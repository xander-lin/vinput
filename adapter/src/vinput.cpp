// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx/inputcontext.h>    // InputContext
#include <fcitx/inputpanel.h>       // InputPanel, setClientPreedit
#include <fcitx/inputcontextmanager.h>  // findByUUID
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
#include <mutex>
#include <vector>
#include <spawn.h>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <fstream>

// Vinput ASR provider 接口
#include "asr_provider.h"
#include "mock_provider.h"         // 确保 Mock 后端被链接并自动注册
#include "doubao_provider.h"      // 确保豆包后端被链接并自动注册
#include "qwen_provider.h"        // 确保千问后端被链接并自动注册
#include "audio_capture.h"
#include "output_handler.h"
#include "vinput_config.h"

// notifications addon 公共 API (跨 addon 调用, 仅用于显示切换信息)
#include <fcitx-module/notifications/notifications_public.h>

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

// 配置: 定义 addon 的可配置选项
FCITX_CONFIGURATION(
    VinputConfig,
    fcitx::Option<std::string> defaultProvider{
        this, "DefaultProvider", _("Default ASR Provider"), "zipformer"};
);

// VinputAddon — Vinput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class VinputAddon : public fcitx::AddonInstance {
public:
    VinputAddon(fcitx::Instance *instance) : instance_(instance) {
        reloadConfig();

        auto vjson = vinput::readConfigFile("vinput.json");
        if (!vjson.empty()) {
            activationUsec_ = (uint64_t)vinput::jsonInt(vjson, "activation_msec", 300) * 1000;
            notificationTimeout_ = vinput::jsonInt(vjson, "notification_timeout", 2000);
            debounceCount_ = vinput::jsonInt(vjson, "debounce_count", 2);
        }

        FCITX_INFO() << "Vinput addon loaded";

        // 创建常驻 uinput 虚键盘, 用于还原 CapsLock
        initUinput();

        outputHandler_ = std::make_unique<vinput::OutputHandler>(instance_);

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
    uint64_t activationUsec_ = 300 * 1000;  // from vinput.json: activation_msec
    int notificationTimeout_ = 2000;         // from vinput.json: notification_timeout
    int debounceCount_ = 2;                   // from vinput.json: debounce_count

    // 创建常驻 uinput 虚拟键盘设备, 用于还原 CapsLock
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

    // 还原 CapsLock — 通过 uinput 虚键发送 CapsLock (还原 LED, 会触发 IM 切换但马上恢复)
    void revertCapsLock() {
        if (uinputFd_ < 0) return;

        struct input_event ev = {};
        ev.type = EV_KEY;
        ev.code = KEY_CAPSLOCK;
        ev.value = 1;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        (void)!write(uinputFd_, &ev, sizeof(ev));
        FCITX_INFO() << "Vinput revert CapsLock via uinput";
    }

    fcitx::Instance *instance_;
    VinputConfig config_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    int uinputFd_ = -1;
    int revertDebounce_ = 0;            // uinput CapsLock 反弹去抖计数

    // Output: encapsulates self-pipe, commit, niri focus switching
    std::unique_ptr<vinput::OutputHandler> outputHandler_;

    // 运行时依赖: notifications addon (仅用于切换显示)
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 提示音: 用系统命令播放 WAV
    static void playSound(const std::string &name) {
        auto path = expandPath("~/.local/share/vinput/sounds/" + name + ".wav");
        if (access(path.c_str(), R_OK) != 0) return;

        // paplay 需要 PULSE_RUNTIME_PATH 环境变量
        const char *pulsePath = getenv("PULSE_RUNTIME_PATH");
        const char *xdgRuntime = getenv("XDG_RUNTIME_DIR");
        std::string paEnv;
        if (pulsePath) paEnv = std::string("PULSE_RUNTIME_PATH=") + pulsePath;
        else if (xdgRuntime) paEnv = std::string("PULSE_RUNTIME_PATH=") + xdgRuntime + "/pulse";

        const char *envp[2] = {paEnv.empty() ? nullptr : paEnv.c_str(), nullptr};
        pid_t pid;
        const char *argv[] = {"paplay", path.c_str(), nullptr};
        posix_spawn(&pid, "/usr/bin/paplay", nullptr, nullptr,
                    (char *const *)argv, envp[0] ? (char *const *)envp : nullptr);
    }

    // 状态
    bool active_ = false;               // 语音录音中
    bool switchActive_ = false;         // Ctrl+CapsLock 切换模式

    // 性能计时
    std::chrono::steady_clock::time_point tPress_, tActivate_, tStop_;
    std::unique_ptr<fcitx::EventSourceTime> timer_;
    std::unique_ptr<vinput::IAsrProvider> asr_;
    std::unique_ptr<vinput::AudioCapture> audioCapture_;
    fcitx::InputContext *currentIC_ = nullptr;
    fcitx::ICUUID currentUuid_ = {};  // 用于 deactivate 后仍能查找 IC
    std::string lastPreeditText_;       // deactivate 时 commit 用
    int providerIndex_ = 0;
    int denoiserIndex_ = 0;

    static const std::vector<std::string>& denoiserList() {
        static const std::vector<std::string> list = {"none", "speexdsp", "deepfilter"};
        return list;
    }

    // 从 keyEvent 提取切换方向, 0 表示非方向键
    static int switchDirection(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Left:  case FcitxKey_h: return -1;
            case FcitxKey_Right: case FcitxKey_l: return  1;
            default: return 0;
        }
    }

    static int switchVertical(const fcitx::KeyEvent &keyEvent) {
        switch (keyEvent.key().sym()) {
            case FcitxKey_Up:   case FcitxKey_k: return -1;
            case FcitxKey_Down: case FcitxKey_j: return  1;
            default: return 0;
        }
    }

    // 仅切换 provider index + 通知 + 持久化, 不创建/启动 ASR 实例
    void doProviderSwitch(int direction) {
        auto list = vinput::AsrProviderRegistry::instance().listFactories();
        if (list.empty()) return;

        providerIndex_ = (providerIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &[nextId, nextName] = list[providerIndex_];

        auto total = (int)list.size();
        auto msg = nextName + " (" + std::to_string(providerIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        FCITX_INFO() << "Vinput switch ASR provider: " << nextName;
        config_.defaultProvider.setValue(nextId);
        safeSaveAsIni(config_, confFile);
        playSound("switch");
    }

    // 切换降噪后端 + 通知 + 持久化
    void doDenoiserSwitch(int direction) {
        auto &list = denoiserList();
        denoiserIndex_ = (denoiserIndex_ + direction + (int)list.size()) % (int)list.size();
        const auto &name = list[denoiserIndex_];

        auto total = (int)list.size();
        auto msg = std::string("Denoiser: ") + name
                   + " (" + std::to_string(denoiserIndex_ + 1)
                   + "/" + std::to_string(total) + ")";
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", msg,
            std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);

        // 持久化到 audio.json
        const char *home = getenv("HOME");
        if (home) {
            std::string path = std::string(home) + "/.config/vinput/audio.json";
            std::string content = "{\"denoise\": \"" + name + "\"}\n";
            FILE *f = fopen(path.c_str(), "w");
            if (f) {
                fwrite(content.c_str(), 1, content.size(), f);
                fclose(f);
            }
        }

        FCITX_INFO() << "Vinput switch denoiser: " << name;
        playSound("switch");
    }

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        bool capsLock = (keyEvent.key().sym() == FcitxKey_Caps_Lock);

        // 放行 CapsLock; 切换模式或录音中放行所有键
        if (!capsLock && !active_ && !switchActive_ && !timer_) return;

        if (keyEvent.isRelease()) {
            if (capsLock) {
                if (revertDebounce_ > 0) {
                    revertDebounce_--;
                    return;
                }
                if (active_) {
                    onDeactivate();
                } else if (switchActive_) {
                    switchActive_ = false;
                    revertDebounce_ = debounceCount_;
                    playSound("deactivate");
                    revertCapsLock();
                } else {
                    timer_.reset();
                }
                keyEvent.filterAndAccept();
            }
            return;
        }

        // ---- 按下事件 ----
        if (capsLock) {
            if (revertDebounce_ > 0) {
                revertDebounce_--;
                return;
            }
            if (timer_ || active_ || switchActive_) return;
            tPress_ = std::chrono::steady_clock::now();
            if (outputHandler_) outputHandler_->setPressTime(tPress_);
            currentIC_ = keyEvent.inputContext();
            if (currentIC_) {
                currentUuid_ = currentIC_->uuid();
                FCITX_INFO() << "Vinput [press] ic=" << currentIC_
                             << " program=" << currentIC_->program()
                             << " frontend=" << currentIC_->frontendName();
            } else {
                FCITX_INFO() << "Vinput [press] no input context";
            }

            bool ctrlHeld = (keyEvent.key().states().toInteger() & (uint32_t)fcitx::KeyState::Ctrl) != 0;

            if (ctrlHeld) {
                // Ctrl+CapsLock: 进入切换模式 (不启用录音)
                switchActive_ = true;
                FCITX_INFO() << "Vinput switch mode active";

                auto list = vinput::AsrProviderRegistry::instance().listFactories();
                if (!list.empty()) {
                    auto &dnList = denoiserList();
                    int di = denoiserIndex_;
                    if (di < 0 || di >= (int)dnList.size()) di = 0;
                    auto msg = std::string("ASR: ") + list[providerIndex_].second
                               + " (" + std::to_string(providerIndex_ + 1)
                               + "/" + std::to_string((int)list.size()) + ")\n"
                               + "Denoiser: " + dnList[di]
                               + " (" + std::to_string(di + 1)
                               + "/" + std::to_string((int)dnList.size()) + ")";
                    notifications()->call<fcitx::INotifications::sendNotification>(
                        "fcitx5-vinput", 0, "fcitx-vinput",
                        "Vinput", msg,
                        std::vector<std::string>{}, notificationTimeout_, nullptr, nullptr);
                }
            } else {
                // 普通 CapsLock: 启动长按计时器
                timer_ = instance_->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    fcitx::now(CLOCK_MONOTONIC) + activationUsec_, 0,
                    [this](fcitx::EventSourceTime *, uint64_t) {
                        onActivate();
                        return false;
                    });
            }
            keyEvent.filterAndAccept();
            return;
        }

        // 切换模式下: 箭头/h/l/j/k 键切换 (可多次)
        if (switchActive_) {
            int dir = switchDirection(keyEvent);
            if (dir != 0) {
                doProviderSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
            dir = switchVertical(keyEvent);
            if (dir != 0) {
                doDenoiserSwitch(dir);
                keyEvent.filterAndAccept();
                return;
            }
        }
    }

    // 长按 500ms 后触发
    void onActivate() {
        timer_.reset();
        tActivate_ = std::chrono::steady_clock::now();
        auto pressMs = std::chrono::duration_cast<std::chrono::milliseconds>(tActivate_ - tPress_).count();
        FCITX_INFO() << "Vinput activated (press→activate=" << pressMs << "ms)";

        // 重新捕获当前焦点窗口 (比 KeyEvent::inputContext 更可靠)
        auto *ic = instance_->mostRecentInputContext();
        if (ic) {
            currentIC_ = ic;
            currentUuid_ = ic->uuid();
            FCITX_INFO() << "Vinput [activate] ic=" << ic
                         << " program=" << ic->program()
                         << " frontend=" << ic->frontendName();
        } else {
            FCITX_INFO() << "Vinput [activate] no input context";
            return;
        }

        // 捕获当前焦点窗口 (通过 OutputHandler 的桌面策略)
        if (outputHandler_) outputHandler_->captureCurrentWindow();
        FCITX_INFO() << "Vinput [activate] captured window";

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

        if (!asr_) {
            FCITX_INFO() << "Vinput: failed to create ASR provider";
            return;
        }

        active_ = true;
        applyAsrConfig();
        setupAsrCallbacks();

        audioCapture_ = std::make_unique<vinput::AudioCapture>();
        {
            // 从 audio.json 读取初始降噪方法，设置到 AudioCapture
            const char *home = getenv("HOME");
            if (home) {
                std::string path = std::string(home) + "/.config/vinput/audio.json";
                std::ifstream f(path);
                if (f) {
                    std::string json((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
                    auto pos = json.find("\"denoise\"");
                    if (pos != std::string::npos) {
                        pos = json.find('"', json.find(':', pos) + 1);
                        if (pos != std::string::npos) {
                            pos++;
                            auto end = json.find('"', pos);
                            if (end != std::string::npos) {
                                auto method = json.substr(pos, end - pos);
                                auto &list = denoiserList();
                                for (int i = 0; i < (int)list.size(); i++) {
                                    if (list[i] == method) { denoiserIndex_ = i; break; }
                                }
                            }
                        }
                    }
                }
            }
        }
        audioCapture_->setRecordedCallback([this](const std::vector<int16_t> &samples, const std::string &wav) {
            if (asr_) asr_->transcribe(samples, wav);
        });
        audioCapture_->setStateCallback([](bool active) {
            FCITX_INFO() << "Vinput ASR state: " << (active ? "on" : "off");
        });
        audioCapture_->setStatusTextCallback([this](const std::string &text) {
            if (outputHandler_) outputHandler_->showStatus(text);
        });

        playSound("activate");
        audioCapture_->start();
    }

    // 注册 ASR 回调
    void setupAsrCallbacks() {
        if (!asr_) return;
        auto tPress = tPress_;
        asr_->setResultCallback([this, tPress](const std::string &text, bool isFinal) {
            auto tResult = std::chrono::steady_clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tPress).count();
            fprintf(stderr, "Vinput [timer] press→result=%ldms\n", ms);
            FCITX_INFO() << "Vinput ASR result: " << text
                         << " (final=" << isFinal << ")";
            if (outputHandler_) outputHandler_->submit(text);
        });
        asr_->setErrorCallback([this](const std::string &error) {
            onAsrError(error);
        });
    }

    // 注入配置到 ASR 后端
    void applyAsrConfig() {
        if (!asr_) return;
        // 后端配置通过 setConfig() 注入
        // 目前 zipformer 后端使用默认模型路径
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        tStop_ = std::chrono::steady_clock::now();
        auto recMs = std::chrono::duration_cast<std::chrono::milliseconds>(tStop_ - tActivate_).count();
        FCITX_INFO() << "Vinput deactivated (record=" << recMs << "ms)";

        if (audioCapture_) {
            audioCapture_->stop();
            // transcribe already triggered from RecordedCallback during stop()
            audioCapture_.reset();
        }
        asr_.reset();
        currentIC_ = nullptr;

        // commit 最后收到的 preedit 文本
        if (!lastPreeditText_.empty()) {
            auto *ic = instance_->inputContextManager().findByUUID(currentUuid_);
            if (ic) {
                ic->inputPanel().reset();
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
                ic->commitString(lastPreeditText_);
                FCITX_INFO() << "Vinput final commit: \"" << lastPreeditText_ << "\"";
            }
            lastPreeditText_.clear();
        }

        playSound("deactivate");  // 结束音: 低音
        // 松键后还原 CapsLock (按下时硬件层已切换, 现在补一个假按键还原)
        revertDebounce_ = debounceCount_;
        revertCapsLock();
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
