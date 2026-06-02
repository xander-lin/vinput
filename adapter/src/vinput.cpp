// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx/inputcontext.h>    // InputContext
#include <fcitx-utils/event.h>     // EventLoop, addTimeEvent
#include <fcitx-utils/key.h>       // Key
#include <fcitx-utils/keysym.h>    // FcitxKey_Caps_Lock 等键值常量
#include <fcitx-utils/log.h>       // 日志宏 FCITX_INFO/FCITX_DEBUG 等

// notifications addon 公共 API (跨 addon 调用)
#include <fcitx-module/notifications/notifications_public.h>

// Vinput ASR provider 接口
#include "asr_provider.h"
#include "mock_provider.h"

// VinputAddon — Vinput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class VinputAddon : public fcitx::AddonInstance {
public:
    VinputAddon(fcitx::Instance *instance) : instance_(instance) {
        FCITX_INFO() << "Vinput addon loaded";

        // 注册 Mock ASR 后端到 Registry（后续替换为真实后端）
        vinput::AsrProviderRegistry::instance().registerFactory(
            std::make_unique<vinput::MockAsrProviderFactory>());

        // 在 PreInputMethod 阶段监听键盘事件 (早于输入法引擎)
        // 用于拦截 CapsLock 长按触发语音输入
        keyWatcher_ = instance_->watchEvent(
            fcitx::EventType::InputContextKeyEvent,
            fcitx::EventWatcherPhase::PreInputMethod,
            [this](fcitx::Event &event) {
                auto &keyEvent = static_cast<fcitx::KeyEvent &>(event);
                onKeyEvent(keyEvent);
            });
    }

private:
    // 长按阈值 (微秒)
    static constexpr uint64_t kLongPressUsec = 500 * 1000; // 500ms

    fcitx::Instance *instance_;
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> keyWatcher_;

    // 运行时依赖: notifications addon, 首次调用时自动加载
    FCITX_ADDON_DEPENDENCY_LOADER(notifications, instance_->addonManager());

    // 状态
    bool active_ = false;                              // 语音输入是否激活
    std::unique_ptr<fcitx::EventSourceTime> timer_;    // 长按定时器
    std::unique_ptr<vinput::IAsrProvider> asr_;        // ASR 后端实例
    fcitx::InputContext *currentIC_ = nullptr;         // 当前输入上下文

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        if (keyEvent.key().sym() != FcitxKey_Caps_Lock) return;

        if (keyEvent.isRelease()) {
            if (active_) {
                onDeactivate();
            } else {
                timer_.reset();
            }
            keyEvent.filterAndAccept();
        } else {
            if (timer_ || active_) return;
            currentIC_ = keyEvent.inputContext();
            timer_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, kLongPressUsec, 0,
                [this](fcitx::EventSourceTime *source, uint64_t usec) {
                    onActivate();
                    return false;
                });
            keyEvent.filterAndAccept();
        }
    }

    // 长按 500ms 后触发: 创建 ASR 后端并开始录音
    void onActivate() {
        timer_.reset();
        active_ = true;
        FCITX_INFO() << "Vinput activated";

        // 创建 ASR 后端 (后续根据配置选择)
        asr_ = vinput::AsrProviderRegistry::instance().create("mock");
        if (!asr_) {
            FCITX_INFO() << "Vinput: no ASR provider available";
            return;
        }

        // 注册 ASR 回调
        asr_->setResultCallback([this](const std::string &text, bool isFinal) {
            onAsrResult(text, isFinal);
        });
        asr_->setErrorCallback([this](const std::string &error) {
            onAsrError(error);
        });
        asr_->setStateCallback([](bool active) {
            FCITX_INFO() << "Vinput ASR state: " << (active ? "on" : "off");
        });

        // 开始录音 & 识别
        asr_->start();

        // 发送 DBus 通知
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", "语音输入已激活",
            std::vector<std::string>{}, 3000, nullptr, nullptr);
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        FCITX_INFO() << "Vinput deactivated";

        // 停止 ASR
        if (asr_) {
            asr_->stop();
            asr_.reset();
        }
        currentIC_ = nullptr;

        // 发送 DBus 通知
        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", "语音输入已结束",
            std::vector<std::string>{}, 3000, nullptr, nullptr);
    }

    // ASR 识别结果回调
    void onAsrResult(const std::string &text, bool isFinal) {
        FCITX_INFO() << "Vinput ASR result: " << text
                     << " (final=" << isFinal << ")";

        // 最终结果: 提交文本到当前应用
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
// fcitx5 通过 FCITX_ADDON_FACTORY 宏找到此类, 调用 create() 创建 VinputAddon
class VinputFactory : public fcitx::AddonFactory {
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new VinputAddon(manager->instance());
    }
};

// 将 VinputFactory 注册为 fcitx5 addon 入口
// fcitx5 加载共享库时查找此符号, 据此创建 addon 实例
FCITX_ADDON_FACTORY(VinputFactory);
