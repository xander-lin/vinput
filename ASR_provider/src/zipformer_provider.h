#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

class ZipformerAsrProvider : public IAsrProvider {
public:
    ZipformerAsrProvider();

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void runTranscribe(const std::string &wav, const std::string &dir,
                       AsrResultCallback onR, AsrErrorCallback onE);

    std::string modelDir_;
};

class ZipformerAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "zipformer"; }
    std::string name() const override { return "Zipformer (sherpa-onnx)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
