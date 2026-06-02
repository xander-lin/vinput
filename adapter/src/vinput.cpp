// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx/inputcontext.h>    // InputContext
#include <fcitx-config/configuration.h>   // FCITX_CONFIGURATION
#include <fcitx-config/iniparser.h>       // readAsIni, safeSaveAsIni
#include <fcitx-utils/i18n.h>             // _() translation macro
#include <fcitx-utils/event.h>     // EventLoop, addTimeEvent
#include <fcitx-utils/eventloopinterface.h> // now()
#include <fcitx-utils/key.h>       // Key
#include <fcitx-utils/keysym.h>    // FcitxKey_Caps_Lock 等键值常量
#include <fcitx-utils/log.h>       // 日志宏 FCITX_INFO/FCITX_DEBUG 等

// uinput: 内核级常驻虚键设备, 兼容所有 Wayland compositor
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/uinput.h>
#include <string.h>

// notifications addon 公共 API (跨 addon 调用)
#include <fcitx-module/notifications/notifications_public.h>

// Vinput ASR provider 接口
#include "asr_provider.h"
#include "mock_provider.h"         // 确保 Mock 后端被链接并自动注册

// 配置: 定义 addon 的可配置选项
FCITX_CONFIGURATION(
    VinputConfig,
    fcitx::Option<std::string> defaultProvider{
        this, "DefaultProvider", _("Default ASR Provider"), "mock"};
    fcitx::Option<std::string> vllmUds{
        this, "VllmUds", _("vLLM Unix Socket"), ""};
    fcitx::Option<std::string> vllmHost{
        this, "VllmHost", _("vLLM Host"), "127.0.0.1"};
    fcitx::Option<int, fcitx::IntConstrain> vllmPort{
        this, "VllmPort", _("vLLM Port"), 0,
        fcitx::IntConstrain(0, 65535)};
);

// VinputAddon — Vinput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class VinputAddon : public fcitx::AddonInstance {
public:
    VinputAddon(fcitx::Instance *instance) : instance_(instance) {
        reloadConfig();
        FCITX_INFO() << "Vinput addon loaded";

        // 创建常驻 uinput 虚键盘, 用于还原 CapsLock
        initUinput();

        // 在 PreInputMethod 阶段监听键盘事件 (早于输入法引擎)
        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
    }

    ~VinputAddon() override {
        if (uinputFd_ >= 0) {
            ioctl(uinputFd_, UI_DEV_DESTROY);
            close(uinputFd_);
        }
    }

    // 配置读写
    void reloadConfig() override {
        readAsIni(config_, confFile);
    }
    const fcitx::Configuration *getConfig() const override {
        return &config_;
    }
    void setConfig(const fcitx::RawConfig &config) override {
        config_.load(config, true);
        safeSaveAsIni(config_, confFile);
    }

private:
    static constexpr char confFile[] = "conf/vinput.conf";
    static constexpr uint64_t kLongPressUsec = 500 * 1000; // 500ms

    // 还原 CapsLock 状态 — 通过常驻 uinput 虚键发送 CapsLock 按键
    void revertCapsLock() {
        if (uinputFd_ < 0) return;

        struct input_event ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 1;
        write(uinputFd_, &ev, sizeof(ev));
        ev.value = 0;
        write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(uinputFd_, &ev, sizeof(ev));
        FCITX_INFO() << "Vinput revert CapsLock via uinput";
    }

    // 创建常驻 uinput 虚拟键盘设备
    void initUinput() {
        uinputFd_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (uinputFd_ < 0) {
            FCITX_INFO() << "Vinput: cannot open /dev/uinput";
            return;
        }
        ioctl(uinputFd_, UI_SET_EVBIT, EV_KEY);
        ioctl(uinputFd_, UI_SET_KEYBIT, KEY_CAPSLOCK);
        ioctl(uinputFd_, UI_SET_EVBIT, EV_LED);
        ioctl(uinputFd_, UI_SET_LEDBIT, LED_CAPSL);

        struct uinput_setup usetup = {};
        strcpy(usetup.name, "Vinput vkbd");
        usetup.id.bustype = BUS_VIRTUAL;
        ioctl(uinputFd_, UI_DEV_SETUP, &usetup);
        ioctl(uinputFd_, UI_DEV_CREATE);
        FCITX_INFO() << "Vinput uinput device created";
    }

    fcitx::Instance *instance_;
    VinputConfig config_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    int uinputFd_ = -1;

    // 运行时依赖: notifications addon
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 状态
    bool active_ = false;
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<vinput::IAsrProvider> asr_;
    fcitx::InputContext *currentIC_ = nullptr;
    int providerIndex_ = 0; // 当前使用的 ASR 后端在列表中的索引

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        if (!active_ && keyEvent.key().sym() != FcitxKey_Caps_Lock) return;

        if (keyEvent.isRelease()) {
            if (keyEvent.key().sym() == FcitxKey_Caps_Lock) {
                if (active_) {
                    onDeactivate();
                } else {
                    timer_.reset();
                }
                keyEvent.filterAndAccept();
            }
            return;
        }

        // ---- 按下事件 ----
        if (keyEvent.key().sym() == FcitxKey_Caps_Lock) {
            if (timer_ || active_) return;
            currentIC_ = keyEvent.inputContext();
            timer_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC,
                fcitx::now(CLOCK_MONOTONIC) + kLongPressUsec, 0,
                [this](fcitx::EventSourceTime *, uint64_t) {
                    onActivate();
                    return false;
                });
            keyEvent.filterAndAccept();
            return;
        }

        // 激活状态下, 处理左右方向键 / h/l 切换 ASR 后端
        if (active_) {
            if (switchProvider(keyEvent)) return;
        }
    }

    // 切换 ASR 后端
    bool switchProvider(fcitx::KeyEvent &keyEvent) {
        int sym = keyEvent.key().sym();

        int direction = 0;
        if (sym == FcitxKey_Left || sym == FcitxKey_h) {
            direction = -1;
        } else if (sym == FcitxKey_Right || sym == FcitxKey_l) {
            direction = 1;
        } else {
            return false;
        }

        auto list = vinput::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) return false;

        providerIndex_ = (providerIndex_ + direction + (int)list.size()) % (int)list.size();

        // 停止当前后端, 创建新后端
        if (asr_) {
            asr_->stop();
            asr_.reset();
        }

        const auto &[id, name] = list[providerIndex_];
        FCITX_INFO() << "Vinput switch ASR provider: " << name;
        asr_ = vinput::AsrProviderRegistry::instance().create(id);

        if (asr_) {
            applyAsrConfig();
            setupAsrCallbacks();
            asr_->start();
        }

        // 保存为默认
        config_.defaultProvider.setValue(id);
        safeSaveAsIni(config_, confFile);

        // 通知用户当前后端
        auto total = (int)list.size();
        auto msg = name + " (" + std::to_string(providerIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", msg,
            std::vector<std::string>{}, 2000, nullptr, nullptr);

        keyEvent.filterAndAccept();
        return true;
    }

    // 长按 500ms 后触发
    void onActivate() {
        timer_.reset();
        active_ = true;
        FCITX_INFO() << "Vinput activated";

        auto list = vinput::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) {
            FCITX_INFO() << "Vinput: no ASR provider registered";
            return;
        }

        // 根据配置中的默认后端 ID 查找索引
        const auto &defaultId = config_.defaultProvider.value();
        for (int i = 0; i < (int)list.size(); i++) {
            if (list[i].first == defaultId) {
                providerIndex_ = i;
                break;
            }
        }

        asr_ = vinput::AsrProviderRegistry::instance().create(
            list[providerIndex_].first);

        if (asr_) {
            applyAsrConfig();
            setupAsrCallbacks();

            notifications()->call<fcitx::INotifications::sendNotification>(
                "fcitx5-vinput", 0, "fcitx-vinput",
                "Vinput", "语音输入已激活",
                std::vector<std::string>{}, 3000, nullptr, nullptr);

            asr_->start();
        }
    }

        // 注册 ASR 回调
    void setupAsrCallbacks() {
        if (!asr_) return;
        asr_->setResultCallback([this](const std::string &text, bool isFinal) {
            onAsrResult(text, isFinal);
        });
        asr_->setErrorCallback([this](const std::string &error) {
            onAsrError(error);
        });
        asr_->setStateCallback([](bool active) {
            FCITX_INFO() << "Vinput ASR state: " << (active ? "on" : "off");
        });
    }

    // 注入配置到 ASR 后端
    void applyAsrConfig() {
        if (!asr_) return;
        asr_->setConfig("uds", config_.vllmUds.value());
        asr_->setConfig("host", config_.vllmHost.value());
        asr_->setConfig("port", std::to_string(config_.vllmPort.value()));
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        FCITX_INFO() << "Vinput deactivated";

        if (asr_) {
            asr_->stop();
            asr_.reset();
        }
        currentIC_ = nullptr;

        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", "语音输入已结束",
            std::vector<std::string>{}, 3000, nullptr, nullptr);

        // 松键后还原 CapsLock (按下时硬件层已切换, 现在补一个假按键还原)
        revertCapsLock();
    }

    // ASR 识别结果回调
    void onAsrResult(const std::string &text, bool isFinal) {
        FCITX_INFO() << "Vinput ASR result: " << text
                     << " (final=" << isFinal << ")";
        if (isFinal && currentIC_) {
            currentIC_->commitString(text);
            currentIC_->updateUserInterface(
                fcitx::UserInterfaceComponent::InputPanel);
        }
    }

    // ASR 错误回调
    void onAsrError(const std::string &error) {
        FCITX_INFO() << "Vinput ASR error: " << error;
    }
};

// VinputFactory — Vinput 插件的工厂类
class VinputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new VinputAddon(manager->instance());
    }
};

FCITX_ADDON_FACTORY(VinputFactory);
