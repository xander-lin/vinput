// fcitx5 核心头文件
#include <fcitx/addonfactory.h>    // AddonFactory 基类
#include <fcitx/addoninstance.h>   // AddonInstance 基类
#include <fcitx/addonmanager.h>    // AddonManager, 用于获取 fcitx 实例
#include <fcitx/instance.h>        // Instance, fcitx 服务器实例
#include <fcitx/event.h>           // KeyEvent
#include <fcitx-utils/event.h>     // EventLoop, addTimeEvent
#include <fcitx-utils/key.h>       // Key
#include <fcitx-utils/keysym.h>    // FcitxKey_Caps_Lock 等键值常量
#include <fcitx-utils/log.h>       // 日志宏 FCITX_INFO/FCITX_DEBUG 等

// notifications addon 公共 API (跨 addon 调用)
#include <fcitx-module/notifications/notifications_public.h>

// VinputAddon — Vinput 语音输入插件的 addon 主体
// 继承 AddonInstance, fcitx5 加载 addon 时实例化此类
class VinputAddon : public fcitx::AddonInstance {
public:
    VinputAddon(fcitx::Instance *instance) : instance_(instance) {
        // addon 被加载时输出日志, 用于验证 addon 加载成功
        FCITX_INFO() << "Vinput addon loaded";

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

    // 键盘事件回调
    void onKeyEvent(fcitx::KeyEvent &keyEvent) {
        if (keyEvent.key().sym() != FcitxKey_Caps_Lock) return;

        if (keyEvent.isRelease()) {
            // 松键
            if (active_) {
                // 已激活: 结束语音输入
                onDeactivate();
            } else {
                // 未激活: 取消定时器
                timer_.reset();
            }
            // 长按后松键仍拦截, 避免 CapsLock 状态切换
            keyEvent.filterAndAccept();
        } else {
            // 按下: 启动 500ms 定时器
            if (timer_ || active_) return; // 已在等待或已激活
            timer_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, kLongPressUsec, 0,
                [this](fcitx::EventSourceTime *source, uint64_t usec) {
                    onActivate();
                    return false; // 一次性定时器
                });
            keyEvent.filterAndAccept();
        }
    }

    // 长按 500ms 后触发
    void onActivate() {
        timer_.reset();
        active_ = true;
        FCITX_INFO() << "Vinput activated";

        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", "语音输入已激活",
            std::vector<std::string>{}, 3000, nullptr, nullptr);
    }

    // 松键后结束
    void onDeactivate() {
        active_ = false;
        FCITX_INFO() << "Vinput deactivated";

        notifications()->call<fcitx::INotifications::sendNotification>(
            "fcitx5-vinput", 0, "fcitx-vinput",
            "Vinput", "语音输入已结束",
            std::vector<std::string>{}, 3000, nullptr, nullptr);
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
