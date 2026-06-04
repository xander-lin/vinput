#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vinput {

using AsrResultCallback = std::function<void(const std::string &text, bool isFinal)>;
using AsrErrorCallback = std::function<void(const std::string &error)>;

class IAsrProvider {
public:
    virtual ~IAsrProvider() = default;

    virtual void transcribe(std::vector<int16_t> samples, const std::string &wavPath) = 0;

    virtual void setConfig(const std::string &key, const std::string &value) {}

    void setResultCallback(AsrResultCallback cb) { onResult_ = std::move(cb); }
    void setErrorCallback(AsrErrorCallback cb) { onError_ = std::move(cb); }

protected:
    AsrResultCallback onResult_;
    AsrErrorCallback onError_;
};

class IAsrProviderFactory {
public:
    virtual ~IAsrProviderFactory() = default;
    virtual std::string id() const = 0;
    virtual std::string name() const = 0;
    virtual std::unique_ptr<IAsrProvider> create() = 0;
};

class AsrProviderRegistry {
public:
    static AsrProviderRegistry &instance();

    void registerFactory(std::unique_ptr<IAsrProviderFactory> factory);
    std::vector<std::pair<std::string, std::string>> listFactories() const;
    std::unique_ptr<IAsrProvider> create(const std::string &id) const;

private:
    AsrProviderRegistry() = default;
    std::vector<std::unique_ptr<IAsrProviderFactory>> factories_;
};

} // namespace vinput
