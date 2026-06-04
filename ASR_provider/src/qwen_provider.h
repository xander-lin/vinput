#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

struct pa_simple;

namespace vinput {

class QwenAsrProvider : public IAsrProvider {
public:
    QwenAsrProvider();
    ~QwenAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void recordLoop();
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          AsrResultCallback onR, AsrErrorCallback onE);

    std::string apiKey_;

    std::thread recordThread_;
    std::atomic<bool> stopRequested_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;

    std::string sessionWav_;
    AsrResultCallback sessionOnR_;
    AsrErrorCallback sessionOnE_;
    size_t bufferBytes_{0};
};

class QwenAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen"; }
    std::string name() const override { return "Qwen3-ASR-Flash (Alibaba DashScope)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
