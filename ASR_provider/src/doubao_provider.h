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

class DoubaoAsrProvider : public IAsrProvider {
public:
    DoubaoAsrProvider();
    ~DoubaoAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void keepAliveLoop();
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          AsrResultCallback onR, AsrErrorCallback onE);

    std::string apiKey_;
    std::string resourceId_;

    pa_simple *paStream_ = nullptr;
    std::thread keepAliveThread_;
    std::atomic<bool> keepRunning_{true};

    std::atomic<bool> recording_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::condition_variable stopCv_;
    std::atomic<bool> stopRequested_{false};

    std::string sessionWav_;
    AsrResultCallback sessionOnR_;
    AsrErrorCallback sessionOnE_;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
