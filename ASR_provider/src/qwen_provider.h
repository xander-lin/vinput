#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

class QwenAsrProvider : public IAsrProvider {
public:
    QwenAsrProvider();

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          AsrResultCallback onR, AsrErrorCallback onE);

    std::string apiKey_;
    long timeout_ = 60;
};

class QwenAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen"; }
    std::string name() const override { return "Qwen3-ASR-Flash (Alibaba DashScope)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
