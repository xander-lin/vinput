#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "asr_provider.h"

namespace vinput {

class Qwen3AsrProvider : public IAsrProvider {
public:
    explicit Qwen3AsrProvider(
        const std::string &vllmSocket = "");
    ~Qwen3AsrProvider() override;

    void start() override;
    void stop() override;

private:
    void recordThread();
    void sendToVllm();

    std::string vllmSocket_;
    std::string tempWavPath_;
    std::thread recordThread_;
    std::thread sendThread_;
    std::atomic<bool> recording_{false};
};

class Qwen3AsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen3-asr"; }
    std::string name() const override { return "Qwen3-ASR (vLLM)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
