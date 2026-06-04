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
        FCITX_INFO() << "Vinput addon loaded";

        // 创建常驻 uinput 虚键盘, 用于还原 CapsLock
        initUinput();

        // self-pipe: 后台 ASR 线程安全唤醒主事件循环
        if (pipe(wakePipe_) != 0) {
            FCITX_ERROR() << "Vinput: pipe() failed";
            return;
        }
        fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
        fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);
        wakeWatcher_ = instance_->eventLoop().addIOEvent(
            wakePipe_[0], fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO *, int fd, fcitx::IOEventFlags) -> bool {
                char buf[64];
                while (read(fd, buf, sizeof(buf)) > 0) {}
                std::vector<PendingCommit> batch;
                {
                    std::lock_guard<std::mutex> lk(pendingMutex_);
                    batch.swap(pendingCommits_);
                }
                // 状态文本: 直接上屏
                for (auto &pc : batch) {
                    if (!pc.isStatus) continue;
                    auto *ic = instance_->mostRecentInputContext();
                    if (ic && !pc.text.empty()) ic->commitString(pc.text);
                }
                // 纯状态消息(无实际文本需要提交)直接返回
                bool hasCommit = false;
                for (auto &pc : batch) { if (!pc.isStatus) { hasCommit = true; break; } }
                if (!hasCommit) return true;

                if (capturedWinId_.empty() || batch.empty()) {
                    // 无窗口绑定: 直接提交到当前焦点窗口
                    auto tCommit = std::chrono::steady_clock::now();
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tCommit - tPress_).count();
                    fprintf(stderr, "Vinput [timer] press→commit=%ldms\n", ms);
                    for (auto &pc : batch) {
                        auto *ic = instance_->mostRecentInputContext();
                        if (!ic) {
                            FCITX_INFO() << "Vinput [commit] no focused ic, drop";
                            continue;
                        }
                        FCITX_INFO() << "Vinput [commit] ic=" << ic
                                     << " program=" << ic->program()
                                     << " text=\"" << pc.text << "\"";
                        if (!pc.text.empty() && !pc.isStatus) ic->commitString(pc.text);
                    }
                    return true;
                }

                // niri 窗口绑定: 先切换到捕获窗口 → 提交 → 恢复
                auto capturedId = capturedWinId_;
                auto restoreId = niriGetFocusedId();
                if (restoreId == capturedId) {
                    capturedId.clear();  // 已在目标窗口, 无需跳过
                }
                FCITX_INFO() << "Vinput [niri] captured=" << capturedId
                             << " restore=" << restoreId;

                if (!capturedId.empty()) {
                    niriFocusWindow(capturedId);
                    // 动态等待: 轮询 focused-window 直到焦点确实切到目标窗口
                    bool ready = false;
                    for (int i = 0; i < 50; i++) {  // max 500ms
                        usleep(10000);
                        auto cur = niriGetFocusedId();
                        if (cur == capturedId) { ready = true; break; }
                    }
                    if (!ready) {
                        fprintf(stderr, "Vinput [niri] focus switch timeout after 500ms, committing anyway\n");
                    }
                }

                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - tPress_).count();
                fprintf(stderr, "Vinput [timer] press→niri-commit=%ldms\n", ms);
                auto *ic = instance_->mostRecentInputContext();
                if (ic) {
                    for (auto &pc : batch) {
                        FCITX_INFO() << "Vinput [niri-commit] ic=" << ic
                                     << " program=" << ic->program()
                                     << " text=\"" << pc.text << "\"";
                        if (!pc.text.empty() && !pc.isStatus) ic->commitString(pc.text);
                    }
                }
                // 恢复原窗口焦点
                if (!restoreId.empty() && restoreId != capturedId) {
                    niriFocusWindow(restoreId);
                }
                return true;
            });

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
        if (wakePipe_[0] >= 0) close(wakePipe_[0]);
        if (wakePipe_[1] >= 0) close(wakePipe_[1]);
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
    static constexpr uint64_t kLongPressUsec = 300 * 1000; // 300ms

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
        write(uinputFd_, &ev, sizeof(ev));
        ev.value = 0;
        write(uinputFd_, &ev, sizeof(ev));
        ev.type = EV_SYN;
        ev.code = SYN_REPORT;
        ev.value = 0;
        write(uinputFd_, &ev, sizeof(ev));
        FCITX_INFO() << "Vinput revert CapsLock via uinput";
    }

    fcitx::Instance *instance_;
    VinputConfig config_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;
    int uinputFd_ = -1;
    int revertDebounce_ = 0;            // uinput CapsLock 反弹去抖计数

    // self-pipe: 后台线程安全唤醒主事件循环
    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;
    struct PendingCommit {
        fcitx::ICUUID uuid;
        std::string text;
        bool isFinal = false;
        bool isStatus = false;  // true=仅显示 preedit, 不提交
    };
    std::mutex pendingMutex_;
    std::vector<PendingCommit> pendingCommits_;

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
    std::string capturedWinId_;         // niri window id, 录音激活时捕获

    // 性能计时
    std::chrono::steady_clock::time_point tPress_, tActivate_, tStop_, tCommit_;
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
            std::vector<std::string>{}, 2000, nullptr, nullptr);

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
            std::vector<std::string>{}, 2000, nullptr, nullptr);

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

    // niri IPC: 获取当前焦点窗口 ID
    std::string niriGetFocusedId() {
        FILE *f = popen("niri msg focused-window 2>/dev/null", "r");
        if (!f) return "";
        char buf[256] = {};
        fread(buf, 1, sizeof(buf) - 1, f);
        pclose(f);
        // 解析 "Window ID 11:" 或 "Window ID 11: (focused)"
        const char *p = strstr(buf, "Window ID ");
        if (!p) return "";
        p += 10;
        const char *end = strchr(p, ':');
        if (!end) return "";
        return std::string(p, end - p);
    }

    // niri IPC: 聚焦指定窗口
    void niriFocusWindow(const std::string &id) {
        if (id.empty()) return;
        std::string cmd = "niri msg action focus-window --id " + id;
        pid_t pid;
        const char *argv[] = {"sh", "-c", cmd.c_str(), nullptr};
        posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                    (char *const *)argv, environ);
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
                    revertDebounce_ = 2;
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
                        std::vector<std::string>{}, 2000, nullptr, nullptr);
                }
            } else {
                // 普通 CapsLock: 启动长按计时器
                timer_ = instance_->eventLoop().addTimeEvent(
                    CLOCK_MONOTONIC,
                    fcitx::now(CLOCK_MONOTONIC) + kLongPressUsec, 0,
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
        active_ = true;
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

        // niri 窗口绑定: 记录当前窗口 ID
        capturedWinId_ = niriGetFocusedId();
        FCITX_INFO() << "Vinput [activate] niri window=" << capturedWinId_;

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
            audioCapture_->setStateCallback([](bool active) {
                FCITX_INFO() << "Vinput ASR state: " << (active ? "on" : "off");
            });
            audioCapture_->setStatusTextCallback([this](const std::string &text) {
                std::lock_guard<std::mutex> lk(pendingMutex_);
                pendingCommits_.emplace_back(PendingCommit{currentUuid_, text, false, true});
                char c = 1;
                write(wakePipe_[1], &c, 1);
            });

            playSound("activate");  // 激活音: 高音
            audioCapture_->start();
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
            auto samples = audioCapture_->takeSamples();
            auto wavPath = audioCapture_->wavPath();
            if (asr_ && !samples.empty()) {
                asr_->transcribe(std::move(samples), wavPath);
            }
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
        revertDebounce_ = 2;
        revertCapsLock();
    }

    // ASR 识别结果回调 (可能在后台线程调用)
    void onAsrResult(const std::string &text, bool isFinal) {
        auto tResult = std::chrono::steady_clock::now();
        auto stopToResult = std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tStop_).count();
        auto pressToResult = std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tPress_).count();
        fprintf(stderr, "Vinput [timer] stop→result=%ldms, press→result=%ldms\n",
                stopToResult, pressToResult);
        FCITX_INFO() << "Vinput ASR result: " << text
                     << " (final=" << isFinal << ")";

        {
            std::lock_guard<std::mutex> lk(pendingMutex_);
            pendingCommits_.emplace_back(PendingCommit{currentUuid_, text, isFinal});
        }
        char c = 1;
        write(wakePipe_[1], &c, 1);
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
