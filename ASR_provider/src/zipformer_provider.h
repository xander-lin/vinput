#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <chrono>
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
    void recordLoop();
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          const std::string &dir,
                          AsrResultCallback onR, AsrErrorCallback onE);

    static void normalizeAndWriteWav(std::vector<int16_t> &samples,
                                      const std::string &path);
    static void runTranscribe(const std::string &wav, const std::string &dir,
                               AsrResultCallback onR, AsrErrorCallback onE);

    std::string modelDir_;

    std::thread recordThread_;
    std::atomic<bool> stopRequested_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;

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
