#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace vinput {

// 从 ~/.config/vinput/pa_buffer.json 读取或自动检测硬件 burst 大小
// 首次调用时检测并缓存到配置文件, 后续直接读取
// onStatus: 检测期间回调状态消息(如显示到输入框), 仅实际检测时触发
size_t loadOrDetectBufferBytes(
    std::function<void(const std::string &)> onStatus = nullptr);

// 强制重新检测（忽略缓存）
size_t detectHardwareBurstBytes();

} // namespace vinput
