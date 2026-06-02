#pragma once

#include <string>
#include <thread>
#include <atomic>
#include "asr_provider.h"

namespace vinput {

class Qwen3AsrProvider : public IAsrProvider {
public:
    Qwen3AsrProvider();
    ~Qwen3AsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void recordThread();
    void sendToVllm();
    bool ensureServer();
    bool trySend(const std::string &url, const std::string &udsPath,
                 const std::string &json, std::string &resp);

    // 配置 (由 adapter 注入)
    std::string udsPath_;       // 空 = 自动: $XDG_RUNTIME_DIR/vllm-<uid>.sock
    std::string tcpHost_;       // 空 = 禁用 TCP
    int tcpPort_ = 0;           // 0 = 禁用

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
