#pragma once

#include "asr_provider.h"

namespace vinput {

// MockAsrProvider — 用于测试的模拟 ASR 后端
// start() 后每隔一定时间通过 onResult_ 回传假识别文本
// stop() 后停止
class MockAsrProvider : public IAsrProvider {
public:
    void start() override;
    void stop() override;

private:
    bool running_ = false;
};

// MockAsrProviderFactory — 注册到 Registry
class MockAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "mock"; }
    std::string name() const override { return "Mock (test)"; }
    std::unique_ptr<IAsrProvider> create() override {
        return std::make_unique<MockAsrProvider>();
    }
};

} // namespace vinput
