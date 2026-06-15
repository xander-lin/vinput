# ASR Provider API

## Scope

`ASR_provider` defines the speech recognition backend contract used by the fcitx5 adapter. Providers receive already captured audio and return recognized text. They do not own keyboard events, fcitx5 input contexts, desktop focus switching, or text commit.

Audio capture and preprocessing are handled by `AudioCapture` before a provider is called:

```text
AudioCapture -> samples + wavPath -> IAsrProvider::transcribe() -> onResult/onError
```

## Interface

```cpp
// ASR_provider/src/asr_provider.h

namespace vinput {

using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;
using AsrErrorCallback = std::function<void(const std::string &error)>;

class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;

    virtual void transcribe(std::vector<int16_t> samples,
                            const std::string &wavPath) = 0;

    virtual void setConfig(const std::string &key, const std::string &value);

    void setResultCallback(AsrResultCallback cb);
    void setErrorCallback(AsrErrorCallback cb);
};

class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

} // namespace vinput
```

## Lifecycle

```text
adapter                         AudioCapture                  ASR provider
  |                                  |                              |
  |-- start() ---------------------->| open PulseAudio              |
  |                                  | capture/process/write WAV    |
  |-- stop() ----------------------->| finish capture               |
  |<---------------- samples,wavPath-|                              |
  |-- transcribe(samples,wavPath) -------------------------------->|
  |<------------------------------ onResult(text,true)/onError ----|
  |-- OutputHandler::submit(text)    |                              |
```

## Callbacks

| Callback | Signature | Meaning |
|----------|-----------|---------|
| `onResult` | `(text, isFinal)` | Recognized text. Current providers return final text; streaming providers may use `isFinal=false` for partial results. |
| `onError` | `(error)` | Human-readable provider failure. |

Recording state and status text belong to `AudioCapture`, not `IAsrProvider`.

## Registry

Provider factories self-register during static initialization. The ASR static library is linked with `link_whole` so those registration objects are retained.

```cpp
auto &reg = vinput::AsrProviderRegistry::instance();
auto list = reg.listFactories();
auto asr = reg.create("mock");
```

## Implementations

| ID | Name | Input | External dependency |
|----|------|-------|---------------------|
| `mock` | Mock (test) | generated samples or empty WAV path | none |
| `doubao` | Doubao AUC | WAV path / encoded WAV | API key in `~/.config/vinput/doubao.json` |
| `qwen` | Qwen3-ASR-Flash | WAV path / Data URL | API key in `~/.config/vinput/qwen.json` |
| `zipformer` | Local Zipformer | WAV path / local binary | sherpa-onnx binary and model files |
| `fire_red` | Local FireRed | WAV path / local binary | sherpa-onnx-offline binary and model files |

## Adding A Provider

1. Add `xxx_provider.h` and `xxx_provider.cpp` under `ASR_provider/src/`.
2. Implement `IAsrProvider::transcribe(samples, wavPath)`.
3. Implement `IAsrProviderFactory::id()`, `name()`, and `create()`.
4. Register the factory in the provider `.cpp` with a static initializer.
5. Add the provider source to `ASR_provider/src/meson.build`.
6. Verify with `meson test -C build` and a provider-specific manual test if it depends on network, model files, or hardware.

```cpp
#include "asr_provider.h"

class MyProvider : public vinput::IAsrProvider {
public:
    void transcribe(std::vector<int16_t> samples,
                    const std::string &wavPath) override {
        // Use samples and/or wavPath. Do not open the microphone here.
        if (onResult_) onResult_("recognized text", true);
    }
};

class MyProviderFactory : public vinput::IAsrProviderFactory {
public:
    std::string id() const override { return "my-provider"; }
    std::string name() const override { return "My ASR Provider"; }
    std::unique_ptr<vinput::IAsrProvider> create() override {
        return std::make_unique<MyProvider>();
    }
};

static bool myRegistered = []() {
    vinput::AsrProviderRegistry::instance().registerFactory(
        std::make_unique<MyProviderFactory>());
    return true;
}();
```

## Adapter Integration

The adapter creates a provider at activation time, records audio through `AudioCapture`, then calls `transcribe()` from the recorded callback.

```cpp
asr_ = vinput::AsrProviderRegistry::instance().create(providerId);
asr_->setResultCallback([this](const std::string &text, bool isFinal) {
    if (outputHandler_ && isFinal) outputHandler_->submit(text);
});
asr_->setErrorCallback([this](const std::string &error) {
    onAsrError(error);
});

audioCapture_ = std::make_unique<vinput::AudioCapture>();
audioCapture_->setRecordedCallback([this](const std::vector<int16_t> &samples,
                                           const std::string &wavPath) {
    if (asr_) asr_->transcribe(samples, wavPath);
});
audioCapture_->start();
```

## Verification

Default automated verification does not require microphone hardware or API keys:

```bash
meson test -C build
```

Provider-specific verification is still required for cloud APIs and local model backends because those depend on external credentials, network, binaries, and model files.
