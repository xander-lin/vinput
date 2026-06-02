#include "mock_provider.h"

namespace vinput {

// 自动注册: 程序启动时静态初始化, 将 Mock 后端注册到全局 Registry
static bool _mockRegistered = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MockAsrProviderFactory>());
    return true;
}();

void MockAsrProvider::start() {
    if (running_) return;
    running_ = true;

    if (onState_) onState_(true);

    if (onResult_) {
        onResult_("你好世界", true);
    }
}

void MockAsrProvider::stop() {
    if (!running_) return;
    running_ = false;

    if (onState_) onState_(false);
}

} // namespace vinput
