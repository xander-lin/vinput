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

// X11/XTest: 用于在拦截 CapsLock 后还原系统大写锁定状态
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/extensions/XTest.h>

// uinput: 内核级假按键注入, 兼容所有 Wayland compositor
#include <fcntl.h>
#include <unistd.h>
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
);

// VinputAddon — Vinput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class VinputAddon : public fcitx::AddonInstance {
public:
    VinputAddon(fcitx::Instance *instance) : instance_(instance) {
        reloadConfig();
        FCITX_INFO() << "Vinput addon loaded";

        // 在 PreInputMethod 阶段监听键盘事件 (早于输入法引擎)
        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
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

    // 还原 CapsLock 状态
    // 优先 uinput (内核级, 兼容所有 compositor), 其次 XTest (X11)
    void revertCapsLock() {
        // 方案 1: uinput — 内核虚键注入, 对所有 compositor 生效
        int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            ioctl(fd, UI_SET_EVBIT, EV_KEY);
            ioctl(fd, UI_SET_KEYBIT, KEY_CAPSLOCK);
            ioctl(fd, UI_SET_EVBIT, EV_LED);
            ioctl(fd, UI_SET_LEDBIT, LED_CAPSL);

            struct uinput_setup usetup = {};
            strcpy(usetup.name, "Vinput CapsLock revert");
            usetup.id.bustype = BUS_VIRTUAL;
            ioctl(fd, UI_DEV_SETUP, &usetup);
            ioctl(fd, UI_DEV_CREATE);

            // compositor 需要时间识别虚设备, 稍等再发键
            usleep(10000); // 10ms

            struct input_event ev = {};
            ev.type = EV_KEY;
            ev.code = KEY_CAPSLOCK;
            ev.value = 1;
            write(fd, &ev, sizeof(ev));
            ev.value = 0;
            write(fd, &ev, sizeof(ev));
            ev.type = EV_SYN;
            ev.code = SYN_REPORT;
            ev.value = 0;
            write(fd, &ev, sizeof(ev));

            // compositor 处理完事件后再销毁 (modifier 切换需要时间)
            usleep(200000); // 200ms

            ioctl(fd, UI_DEV_DESTROY);
            close(fd);
            FCITX_INFO() << "Vinput revert CapsLock via uinput";
            return;
        }

        // 方案 2: XTest — X11/XWayland 回退方案
        auto *display = XOpenDisplay(nullptr);
        if (display) {
            auto keycode = XKeysymToKeycode(display, XK_Caps_Lock);
            if (keycode) {
                FCITX_INFO() << "Vinput revert CapsLock via XTest";
                XTestFakeKeyEvent(display, keycode, True, CurrentTime);
                XTestFakeKeyEvent(display, keycode, False, CurrentTime);
                XFlush(display);
            }
            XCloseDisplay(display);
        }
    }

    fcitx::Instance *instance_;
    VinputConfig config_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;

    // 运行时依赖: notifications addon
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 状态
    bool active_ = false;
    bool reverting_ = false;  // 防止 forwardKey 触发的 CapsLock 事件递归
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<vinput::IAsrProvider> asr_;
    fcitx::InputContext *currentIC_ = nullptr;
    int providerIndex_ = 0; // 当前使用的 ASR 后端在列表中的索引

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        if (reverting_ || (!active_ && keyEvent.key().sym() != FcitxKey_Caps_Lock)) return;

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
