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
    void keepAliveLoop();
    bool wsConnect();
    void wsSendFrame(uint8_t msgType, uint8_t flags, const void *payload, uint32_t len);
    bool wsRecvFrame(std::string &out);
    bool parseResponse(const std::string &json, std::string &text, bool &definite);

    std::string apiKey_;
    std::string resourceId_;
    std::string tempWavPath_;

    // 常驻 PA 流
    pa_simple *paStream_ = nullptr;
    std::thread keepAliveThread_;
    std::atomic<bool> keepAliveRunning_{true};
    std::atomic<bool> sendRunning_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::vector<int16_t> allSamples_;

    // WebSocket (单线程轮询收发)
    void *curl_ = nullptr;
};

class DoubaoAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "doubao"; }
    std::string name() const override { return "Doubao (ByteDance)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
