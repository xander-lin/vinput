#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <thread>
#include <mutex>
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
    std::string scriptPath_;
    std::string tempWavPath_;

    // 常驻录音流 (单线程 keep-alive + 录音发送)
    pa_simple *paStream_ = nullptr;
    std::thread keepAliveThread_;
    std::atomic<bool> keepAliveRunning_{true};
    std::atomic<bool> sendRunning_{false};
    std::mutex sendMutex_;
    int sendFd_ = -1;
    std::vector<int16_t> allSamples_;

    // 子进程
    int recvFd_ = -1;
    pid_t childPid_ = 0;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
