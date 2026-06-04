#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

class DoubaoAsrProvider : public IAsrProvider {
public:
    DoubaoAsrProvider();

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void processRecording(std::vector<int16_t> samples,
                          const std::string &wavPath,
                          AsrResultCallback onR, AsrErrorCallback onE);

    std::string apiKey_;
    std::string resourceId_;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
