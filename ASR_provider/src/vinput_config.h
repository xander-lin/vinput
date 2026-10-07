#pragma once

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <curl/curl.h>

#include "qwen_json.h"

namespace vinput {

inline std::string configDir() {
    const char *h = getenv("HOME");
    return h ? std::string(h) + "/.config/vinput" : "/tmp/vinput_cfg";
}

inline std::string configPath(const std::string &name) {
    return configDir() + "/" + name;
}

inline std::string readFileIfExists(const std::string &path) {
    std::ifstream f(path);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

inline bool fileExists(const std::string &path) {
    return std::filesystem::exists(path);
}

// Read a config file from ~/.config/vinput/<name>. Defaults live in code, so
// files are optional and sparse. // line comments are stripped (string-aware,
// offsets preserved) before the text reaches validators and field walkers.
inline std::string readConfigFile(const std::string &name) {
    return qjsonStripComments(readFileIfExists(configPath(name)));
}

// Extract a top-level object section (e.g. "audio" of config.json) as raw
// JSON text; "" when the file or the section is absent.
inline std::string readConfigSection(const std::string &name,
                                     const std::string &section) {
    std::string json = readConfigFile(name);
    if (json.empty()) return "";
    return qjsonRawValue(json, section);
}

inline std::string jsonStr(const std::string &json, const std::string &key,
                           const std::string &def = "") {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return def;
    pos = json.find('"', json.find(':', pos) + 1);
    if (pos == std::string::npos) return def;
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return def;
    return json.substr(pos, end - pos);
}

inline int jsonInt(const std::string &json, const std::string &key, int def = 0) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos) + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    char *end = nullptr;
    long val = strtol(json.c_str() + pos, &end, 10);
    if (end == json.c_str() + pos) return def;
    return (int)val;
}

inline double jsonDouble(const std::string &json, const std::string &key, double def = 0.0) {
    auto pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return def;
    pos = json.find(':', pos) + 1;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    char *end = nullptr;
    double val = strtod(json.c_str() + pos, &end);
    if (end == json.c_str() + pos) return def;
    return val;
}

struct CurlHandle {
    CURL *curl;
    CurlHandle() : curl(curl_easy_init()) {}
    ~CurlHandle() { if (curl) curl_easy_cleanup(curl); }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;
    operator CURL*() { return curl; }
};

inline CurlHandle &threadLocalCurlHandle() {
    thread_local CurlHandle handle;
    return handle;
}

inline void evictCurlHandle();

inline CURL* getCurl() {
    auto &handle = threadLocalCurlHandle();
    if (!handle.curl) return nullptr;
    return handle;
}

inline void evictCurlHandle() {
    auto &handle = threadLocalCurlHandle();
    if (handle.curl) {
        curl_easy_cleanup(handle.curl);
        handle.curl = curl_easy_init();
    }
}

inline int cancelCurl(void *clientp, curl_off_t, curl_off_t,
                      curl_off_t, curl_off_t) {
    auto *cancel = static_cast<std::atomic_bool *>(clientp);
    return cancel && cancel->load() ? 1 : 0;
}

class CurlCancellationScope {
public:
    CurlCancellationScope(CURL *curl,
                          std::shared_ptr<std::atomic_bool> cancel)
        : curl_(curl), cancel_(std::move(cancel)) {
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, cancelCurl);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, cancel_.get());
    }

    ~CurlCancellationScope() {
        curl_easy_setopt(curl_, CURLOPT_NOPROGRESS, 1L);
        curl_easy_setopt(curl_, CURLOPT_XFERINFOFUNCTION, nullptr);
        curl_easy_setopt(curl_, CURLOPT_XFERINFODATA, nullptr);
    }

    CurlCancellationScope(const CurlCancellationScope &) = delete;
    CurlCancellationScope &operator=(const CurlCancellationScope &) = delete;

private:
    CURL *curl_;
    std::shared_ptr<std::atomic_bool> cancel_;
};

} // namespace vinput
