# ASR Provider API 文档

## 概述

ASR_provider 定义了语音识别后端的抽象接口。adapter 通过 `IAsrProvider` 接口驱动任意 ASR 后端，无需关心具体实现。

## 接口

```cpp
// ASR_provider/src/asr_provider.h

namespace vinput {

using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;
using AsrErrorCallback  = std::function<void(const std::string &error)>;
using AsrStateCallback  = std::function<void(bool active)>;

class IAsrProvider {
    virtual void start() = 0;   // 开始录音 & 识别
    virtual void stop() = 0;    // 停止

    void setResultCallback(AsrResultCallback cb);  // 注册结果回调
    void setErrorCallback(AsrErrorCallback cb);    // 注册错误回调
    void setStateCallback(AsrStateCallback cb);    // 注册状态回调
};

class IAsrProviderFactory {
    virtual std::string id() const = 0;             // 唯一标识, e.g. "openai-whisper"
    virtual std::string name() const = 0;           // 显示名称, e.g. "OpenAI Whisper"
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

} // namespace vinput
```

## 生命周期

```
adapter                          ASR provider
  │                                   │
  │── create("mock") ────────────────>│  创建实例
  │── setResultCallback(cb) ─────────>│  注册回调
  │── start() ───────────────────────>│  开始录音
  │                                   │── onResult(text, false)  // 中间结果
  │                                   │── onResult(text, true)   // 最终结果
  │── stop() ────────────────────────>│  停止录音
  │                                   │
```

## 回调说明

| 回调 | 签名 | 说明 |
|------|------|------|
| onResult | `(text, isFinal)` | 识别文本，`isFinal=false` 是中间结果（会随更多输入变化），`isFinal=true` 是最终结果 |
| onError | `(error)` | 错误描述 |
| onState | `(active)` | `true` 开始录音，`false` 停止 |

## Registry 注册机制

```cpp
// 在 adapter 初始化时注册所有可用后端
auto &reg = AsrProviderRegistry::instance();
reg.registerFactory(std::make_unique<MyProviderFactory>());

// 获取可用后端列表
auto list = reg.listFactories();  // [{id, name}, ...]

// 根据 ID 创建实例
auto asr = reg.create("openai-whisper");
```

## 已有实现

| ID | 名称 | 状态 |
|----|------|------|
| `mock` | Mock (test) | 已实现，用于测试，start() 立即返回 "你好世界" |

## 如何添加新后端

1. 在 `ASR_provider/src/` 下创建 `xxx_provider.h` 和 `xxx_provider.cpp`
2. 继承 `IAsrProvider` 实现 `start()` / `stop()`
3. 继承 `IAsrProviderFactory` 提供 `id()` / `name()` / `create()`
4. **在 .cpp 末尾加静态注册代码，编译后自动注册：**

```cpp
// 静态初始化：程序启动时自动注册到全局 Registry
static bool _registered = []() {
    vinput::AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MyProviderFactory>());
    return true;
}();
```

无需修改 adapter 代码，编译链接即可被自动发现。

## 示例骨架

```cpp
#include "asr_provider.h"

class MyProvider : public vinput::IAsrProvider {
    void start() override {
        if (onState_) onState_(true);
        // 1. 打开麦克风录音
        // 2. 流式发送音频到 ASR API
        // 3. 收到结果后调用 onResult_(text, isFinal)
        // 4. 出错调用 onError_(msg)
    }
    void stop() override {
        if (onState_) onState_(false);
    }
};

class MyProviderFactory : public vinput::IAsrProviderFactory {
    std::string id() const override { return "my-provider"; }
    std::string name() const override { return "My ASR Provider"; }
    std::unique_ptr<vinput::IAsrProvider> create() override {
        return std::make_unique<MyProvider>();
    }
};

// 自动注册
static bool _myRegistered = []() {
    vinput::AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MyProviderFactory>());
    return true;
}();
```

## 运行时切换

语音输入激活后（CapsLock 长按中），按以下键切换后端：

| 按键 | 操作 |
|------|------|
| ← / h | 上一个后端 |
| → / l | 下一个后端 |

切换时会显示通知（后端名 + 序号），同时自动保存为默认配置。

## adapter 集成方式

adapter 通过 Registry 创建 ASR 实例，生命周期绑定在激活/停止之间：

```cpp
// 激活时
onActivate() {
    asr_ = AsrProviderRegistry::instance().create("mock");
    asr_->setResultCallback([this](auto &text, bool final) {
        if (final) currentIC_->commitString(text);
    });
    asr_->start();
}

// 停止时
onDeactivate() {
    asr_->stop();
    asr_.reset();
}
```
