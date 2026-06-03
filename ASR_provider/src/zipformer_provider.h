#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "asr_provider.h"

namespace vinput {

class ZipformerAsrProvider : public IAsrProvider {
public:
    ZipformerAsrProvider();
    ~ZipformerAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void recordThread();
    void transcribe();   // 子进程调用 sherpa-onnx

    std::string modelDir_;
    std::string tempWavPath_;
    std::thread recordThread_;
    std::atomic<bool> recording_{false};
};

class ZipformerAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "zipformer"; }
    std::string name() const override { return "Zipformer (sherpa-onnx)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
