#pragma once

#include <string>
#include <atomic>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

class DoubaoAsrProvider : public IAsrProvider {
public:
    DoubaoAsrProvider();
    ~DoubaoAsrProvider() override;

    void start() override;
    void stop() override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    void recordAndSend();
    void readResults();
    void writeWav(const std::vector<int16_t> &samples, const std::string &path);

    std::string scriptPath_;
    std::string tempWavPath_;
    std::atomic<bool> running_{false};
    int childStdin_ = -1;
    int childStdout_ = -1;
    pid_t childPid_ = 0;
    std::string partialText_;
    std::string finalText_;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
