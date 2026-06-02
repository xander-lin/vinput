#include "mock_provider.h"

namespace vinput {

void MockAsrProvider::start() {
    if (running_) return;
    running_ = true;

    if (onState_) onState_(true);

    // 模拟: 立即返回一段识别文本
    // 真实实现中，这里会启动录音线程并流式调用 ASR API
    if (onResult_) {
        onResult_("你好世界", true); // isFinal=true
    }
}

void MockAsrProvider::stop() {
    if (!running_) return;
    running_ = false;

    if (onState_) onState_(false);
}

} // namespace vinput
