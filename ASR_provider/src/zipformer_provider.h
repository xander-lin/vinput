#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

struct pa_simple;

namespace vinput {

class ZipformerAsrProvider : public IAsrProvider {
public:
    ZipformerAsrProvider();
    ~ZipformerAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void keepAliveLoop();
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          AsrResultCallback onR, AsrErrorCallback onE);

    static void normalizeAndWriteWav(std::vector<int16_t> &samples,
                                      const std::string &path);
    static void runTranscribe(const std::string &wav, const std::string &dir,
                               AsrResultCallback onR, AsrErrorCallback onE);

    std::string modelDir_;

    // 常驻录音流 (防止 PipeWire 挂起设备)
    pa_simple *paStream_ = nullptr;
    std::thread keepAliveThread_;
    std::atomic<bool> keepRunning_{true};

    // 录音控制
    std::atomic<bool> recording_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::condition_variable stopCv_;
    std::atomic<bool> stopRequested_{false};

    // 当前录音会话参数 (start() 时设置)
    std::string sessionWav_;
    std::string sessionDir_;
    AsrResultCallback sessionOnR_;
    AsrErrorCallback sessionOnE_;
};

class ZipformerAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "zipformer"; }
    std::string name() const override { return "Zipformer (sherpa-onnx)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
