#pragma once

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <curl/curl.h>

namespace vinput {

inline std::string configDir() {
    const char *h = getenv("HOME");
    return h ? std::string(h) + "/.config/vinput" : "/tmp/vinput_cfg";
}

inline std::string configPath(const std::string &name) {
    return configDir() + "/" + name;
}

inline std::string systemConfigDir() {
    return "/etc/vinput";
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

inline void copyFileIfMissing(const std::string &src, const std::string &dst) {
    if (!fileExists(src) || fileExists(dst)) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dst).parent_path(), ec);
    if (ec) return;
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::none, ec);
}

inline std::string readConfigFileFromDirs(const std::string &name,
                                          const std::string &userDir,
                                          const std::string &fallbackDir) {
    auto userPath = userDir + "/" + name;
    if (fileExists(userPath)) return readFileIfExists(userPath);

    auto fallbackPath = fallbackDir + "/" + name;
    copyFileIfMissing(fallbackPath, userPath);
    return readFileIfExists(userPath);
}

inline std::string readConfigFile(const std::string &name) {
    return readConfigFileFromDirs(name, configDir(), systemConfigDir());
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

inline std::string advancedSection(const std::string &key) {
    auto cfg = readConfigFile("advanced.json");
    if (cfg.empty()) return "";
    auto pos = cfg.find("\"" + key + "\"");
    if (pos == std::string::npos) return "";
    auto start = cfg.find('{', pos);
    if (start == std::string::npos) return "";
    auto end = cfg.find('}', start);
    if (end == std::string::npos) return "";
    return cfg.substr(start, end - start + 1);
}

struct CurlHandle {
    CURL *curl;
    CurlHandle() : curl(curl_easy_init()) {}
    ~CurlHandle() { if (curl) curl_easy_cleanup(curl); }
    CurlHandle(const CurlHandle &) = delete;
    CurlHandle &operator=(const CurlHandle &) = delete;
    operator CURL*() { return curl; }
};

inline CURL* getCurl() {
    thread_local CurlHandle handle;
    if (!handle.curl) return nullptr;
    return handle;
}

} // namespace vinput
