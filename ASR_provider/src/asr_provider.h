#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vinput {

// 识别结果回调: text=识别文本, isFinal=true表示最终结果(不再变化)
using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;

// 错误回调: error=错误描述
using AsrErrorCallback = std::function<void(const std::string &error)>;

// 状态变化回调: active=true表示开始录音, false表示停止
using AsrStateCallback = std::function<void(bool active)>;

// IAsrProvider — ASR 语音识别后端抽象接口
// 每个具体后端（OpenAI Whisper、Azure Speech、本地 Vosk/Whisper 等）实现此接口
//
// 生命周期:
//   1. create(...) → 创建实例
//   2. setResultCallback / setErrorCallback / setStateCallback → 注册回调
//   3. start() → 开始录音 & 识别
//   4. onResult(...) 多次回调 → 中间/最终识别结果
//   5. stop() → 停止录音 & 识别
//
class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;

    // 开始录音 & 识别
    virtual void start() = 0;

    // 停止录音 & 识别
    virtual void stop() = 0;

    // 注册结果回调（可在 start 前后调用）
    void setResultCallback(AsrResultCallback cb) { onResult_ = std::move(cb); }

    // 注册错误回调
    void setErrorCallback(AsrErrorCallback cb) { onError_ = std::move(cb); }

    // 注册状态回调
    void setStateCallback(AsrStateCallback cb) { onState_ = std::move(cb); }

protected:
    AsrResultCallback onResult_;  // 子类在识别到文本时调用
    AsrErrorCallback  onError_;   // 子类在出错时调用
    AsrStateCallback  onState_;   // 子类在录音开始/停止时调用
};

// AsrProviderFactory — ASR 后端的工厂基类
// 每种 ASR 后端实现一个对应的 Factory，注册到 Registry 中
//
class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;

    // 唯一标识，如 "openai-whisper", "azure-speech", "local-vosk"
    virtual std::string id() const = 0;

    // 显示名称
    virtual std::string name() const = 0;

    // 创建实例，config 传递后端专属参数（如 API key、模型名等）
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

// AsrProviderRegistry — 全局 ASR 后端注册表
// 在程序启动时由各后端实现调用 registerFactory() 注册自己
// adapter 通过 listFactories() 获取可用后端列表，通过 create() 实例化
//
class AsrProviderRegistry {
public:
    static AsrProviderRegistry &instance();

    // 注册一个工厂
    void registerFactory(std::unique_ptr<IAsrProviderFactory> factory);

    // 获取所有已注册的后端 ID 和名称列表
    std::vector<std::pair<std::string, std::string>> listFactories() const;

    // 根据 ID 创建实例，未找到返回 nullptr
    std::unique_ptr<IAsrProvider> create(const std::string &id) const;

private:
    AsrProviderRegistry() = default;
    std::vector<std::unique_ptr<IAsrProviderFactory>> factories_;
};

} // namespace vinput
