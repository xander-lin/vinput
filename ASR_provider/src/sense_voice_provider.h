#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <memory>
#include "asr_provider.h"

namespace vinput {

class SenseVoiceAsrProvider : public IAsrProvider {
public:
    SenseVoiceAsrProvider();
    ~SenseVoiceAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void recordThread();
    static void recognizeThread(std::string wavPath, void *recognizer,
                                AsrResultCallback onR, AsrErrorCallback onE);

    void *recognizer_ = nullptr;  // sherpa_onnx::OfflineRecognizer*
    std::string modelPath_;
    std::string tokensPath_;
    std::string tempWavPath_;
    std::thread recordThread_;
    std::thread recogThread_;
    std::atomic<bool> recording_{false};
};

class SenseVoiceAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "sense-voice"; }
    std::string name() const override { return "SenseVoice (sherpa-onnx)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
