#include "buffer_detect.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <fstream>
#include <functional>
#include <vector>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>

using Clock = std::chrono::steady_clock;

namespace vinput {

static std::string configPath() {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    std::string dir = std::string(home) + "/.config/vinput";
    mkdir(dir.c_str(), 0755);
    return dir + "/pa_buffer.json";
}

static size_t loadFromConfig() {
    std::ifstream f(configPath());
    if (!f.is_open()) return 0;

    std::string content((std::istreambuf_iterator<char>(f)),
                         std::istreambuf_iterator<char>());
    f.close();

    auto pos = content.find("\"buffer_bytes\"");
    if (pos == std::string::npos) return 0;
    pos = content.find(':', pos);
    if (pos == std::string::npos) return 0;
    while (++pos < content.size() && (content[pos] == ' ' || content[pos] == '\t' || content[pos] == '\n')) {}
    if (pos >= content.size()) return 0;

    size_t val = (size_t)std::stoul(content.substr(pos));
    if (val < 4096 || val > 1048576) return 0;  // invalid range
    return val;
}

static void saveToConfig(size_t bytes) {
    std::ofstream f(configPath());
    if (!f.is_open()) {
        fprintf(stderr, "Vinput: failed to write %s\n", configPath().c_str());
        return;
    }
    f << "{\"buffer_bytes\": " << bytes << "}\n";
    f.close();
    fprintf(stderr, "Vinput: saved buffer=%zu to %s\n", bytes, configPath().c_str());
}

size_t detectHardwareBurstBytes() {
    fprintf(stderr, "Vinput: detecting hardware buffer period...\n");

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "vinput-detect", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput: PA detection error: %s, using default 16384\n", pa_strerror(error));
        return 16384;
    }

    // 200 字节 = 100 samples @16kHz S16LE, 精确 6.25ms 精度
    uint8_t buf[200];
    constexpr size_t kDetectBufSize = sizeof(buf);
    std::vector<double> readMs;

    auto tEnd = Clock::now() + std::chrono::seconds(6);
    fprintf(stderr, "Vinput: recording 6s for detection...\n");

    while (Clock::now() < tEnd) {
        int err = 0;
        auto t0 = Clock::now();
        if (pa_simple_read(pa, buf, kDetectBufSize, &err) < 0) {
            fprintf(stderr, "Vinput: detection read error: %s\n", pa_strerror(err));
            break;
        }
        auto t1 = Clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        readMs.push_back(ms);
    }

    pa_simple_free(pa);

    if (readMs.size() < 3) {
        fprintf(stderr, "Vinput: too few reads (%zu), using default 16384\n", readMs.size());
        return 16384;
    }

    // 自适应判定中断周期:
    // 排序读耗时 → 找相邻最大 gap → 快/慢聚类天然分界
    // gap = sorted[i+1] - sorted[i], maxGap 把数据切成快慢两簇
    // maxGap < 50ms(或占比较小) → 连续流 → 默认 16384
    std::vector<double> sorted = readMs;
    std::sort(sorted.begin(), sorted.end());

    size_t gapIdx = 0;
    double maxGap = 0.0;
    for (size_t i = 0; i + 1 < sorted.size(); i++) {
        double gap = sorted[i + 1] - sorted[i];
        if (gap > maxGap) { maxGap = gap; gapIdx = i; }
    }

    // 连续流判定: gap 太小(全聚在一起) 或 gap 不是数量级跳变(仅时序抖动)
    double slowVal = sorted[gapIdx + 1];
    double fastVal = sorted[gapIdx];
    bool hasBurst = (maxGap > 50.0) && (slowVal > fastVal * 3.0);

    if (!hasBurst) {
        fprintf(stderr, "Vinput: continuous streaming (max_gap=%.0fms min=%.0f max=%.0f) → default 16384\n",
                maxGap, sorted.front(), sorted.back());
        return 16384;
    }

    double thresholdMs = fastVal + maxGap * 0.3;
    double fastBound = thresholdMs * 0.3;
    fprintf(stderr, "Vinput: burst pattern: fast=%.0fms slow=%.0fms gap=%.0fms threshold=%.0fms\n",
            fastVal, slowVal, maxGap, thresholdMs);

    // 分类: >threshold = slow (fill 期), <fastBound = fast (burst 期)
    // 每组 [slow + 后续 fast reads] = 一个硬件周期的完整数据量
    std::vector<size_t> burstSizes;
    size_t currentBurst = 0;
    bool inBurst = false;

    for (size_t i = 0; i < readMs.size(); i++) {
        if (readMs[i] > thresholdMs) {
            if (inBurst && currentBurst > 0) {
                burstSizes.push_back(currentBurst);
                currentBurst = 0;
            }
            inBurst = true;
            currentBurst = kDetectBufSize;
        } else if (readMs[i] < fastBound) {
            if (inBurst) {
                currentBurst += kDetectBufSize;
            } else {
                inBurst = true;
                currentBurst = kDetectBufSize;
            }
        }
    }
    if (inBurst && currentBurst > 0) {
        burstSizes.push_back(currentBurst);
    }

    if (burstSizes.empty()) {
        fprintf(stderr, "Vinput: no burst pattern, using default 16384\n");
        return 16384;
    }

    // 取第一个完整 burst（通常最早记录的 cycle 最准，后续可能受干扰）
    // 但考虑到时序抖动，取最大 burst 做保守估计
    size_t maxBurst = 0;
    fprintf(stderr, "Vinput: detected bursts: ");
    for (size_t i = 0; i < burstSizes.size(); i++) {
        fprintf(stderr, "%zu%s", burstSizes[i], i + 1 < burstSizes.size() ? ", " : "");
        if (burstSizes[i] > maxBurst) maxBurst = burstSizes[i];
    }
    fprintf(stderr, "\n");

    // 向下取整到 1000 的倍数（留安全边际，避免缓冲区略大于 burst）
    size_t result = (maxBurst / 1000) * 1000;
    if (result < 4096) result = 4096;
    if (result > 1048576) result = 1048576;

    fprintf(stderr, "Vinput: buffer set to %zu bytes (%.1f s @16kHz)\n",
            result, result / 32000.0);
    return result;
}

size_t loadOrDetectBufferBytes(std::function<void(const std::string &)> onStatus) {
    size_t cached = loadFromConfig();
    if (cached > 0) {
        fprintf(stderr, "Vinput: using cached buffer=%zu bytes (%.1f s)\n",
                cached, cached / 32000.0);
        return cached;
    }

    if (onStatus) onStatus("Detecting hardware buffer period...");
    size_t detected = detectHardwareBurstBytes();
    saveToConfig(detected);
    if (onStatus) onStatus("");
    return detected;
}

} // namespace vinput
