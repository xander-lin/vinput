#include "qwen_provider.h"
#include "vinput_config.h"

#include <curl/curl.h>
#include <unistd.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <fstream>
#include <memory>

namespace vinput {

static std::string base64Encode(const uint8_t *data, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    return out;
}

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static size_t writeCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static void loadConfig(std::string &apiKey) {
    const char *home = getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/vinput/qwen.json";
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Vinput Qwen: no config at %s\n", path.c_str());
        return;
    }
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    apiKey = jsonGetString(json, "api_key");
}

QwenAsrProvider::QwenAsrProvider() {
    loadConfig(apiKey_);
    auto adv = advancedSection("qwen");
    if (!adv.empty()) {
        timeout_ = (long)jsonInt(adv, "timeout_sec", (int)timeout_);
    }
}

void QwenAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
}

void QwenAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    processRecording(std::move(samples), wavPath, onResult_, onError_);
}

void QwenAsrProvider::processRecording(std::vector<int16_t> samples,
                                         const std::string &wavPath,
                                         AsrResultCallback onR,
                                         AsrErrorCallback onE) {
    fprintf(stderr, "Vinput Qwen: recorded %zu samples to %s\n",
            samples.size(), wavPath.c_str());

    auto apiKey = apiKey_;
    if (apiKey.empty()) {
        if (onE) onE("Qwen: missing api_key in ~/.config/vinput/qwen.json");
        return;
    }

    std::thread([=, this]() {
        auto t0 = std::chrono::steady_clock::now();
        struct Cleanup { std::string p; ~Cleanup() { unlink(p.c_str()); } } _wav{wavPath};

        std::ifstream wf(wavPath, std::ios::binary);
        if (!wf) {
            if (onE) onE("Qwen: failed to read WAV");
            return;
        }
        std::vector<uint8_t> wavData((std::istreambuf_iterator<char>(wf)),
                                      std::istreambuf_iterator<char>());
        if (wavData.empty()) {
            if (onE) onE("Qwen: empty WAV file");
            return;
        }
        std::string b64 = base64Encode(wavData.data(), wavData.size());
        std::string dataUri = "data:audio/wav;base64," + b64;
        auto tEncode = std::chrono::steady_clock::now();

        CURL *curl = getCurl();
        if (!curl) {
            if (onE) onE("Qwen: curl init failed");
            return;
        }
        curl_easy_reset(curl);
        std::string respBody;

        std::string requestBody =
            "{"
            "\"model\":\"qwen3-asr-flash\","
            "\"input\":{"
            "\"messages\":["
            "{\"content\":[{\"audio\":\"" + dataUri + "\"}],\"role\":\"user\"}"
            "]"
            "},"
            "\"parameters\":{"
            "\"asr_options\":{"
            "\"enable_itn\":false"
            "}"
            "}"
            "}";

        struct curl_slist *headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers,
            ("Authorization: Bearer " + apiKey).c_str());

        curl_easy_setopt(curl, CURLOPT_URL,
            "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout_);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);

        auto tNetwork = std::chrono::steady_clock::now();

        fprintf(stderr, "Vinput Qwen: HTTP %ld\n", httpCode);
        fprintf(stderr, "Vinput Qwen: response body: %s\n", respBody.c_str());

        if (res != CURLE_OK || httpCode != 200) {
            fprintf(stderr, "Vinput Qwen: request failed, HTTP %ld, curl=%d\n", httpCode, (int)res);
            if (onE) onE("Qwen: request failed (HTTP " + std::to_string(httpCode) + ")");
            return;
        }

        std::string text;
        auto pos = respBody.find("\"text\":\"");
        if (pos != std::string::npos) {
            pos += 8;
            auto end = respBody.find('"', pos);
            if (end != std::string::npos)
                text = respBody.substr(pos, end - pos);
        }

        auto tParse = std::chrono::steady_clock::now();
        fprintf(stderr, "Vinput Qwen [timer] encode=%ldms network=%ldms parse=%ldms text=\"%s\"\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tNetwork - tEncode).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tNetwork).count(),
                text.c_str());

        if (onR && !text.empty()) {
            onR(text, true);
        } else if (onE) {
            onE("Qwen: empty result");
        }
    }).detach();
}

std::unique_ptr<IAsrProvider> QwenAsrProviderFactory::create() {
    return std::make_unique<QwenAsrProvider>();
}

static struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlInit() { curl_global_cleanup(); }
} _curlInit;

static bool _qwenReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<QwenAsrProviderFactory>());
    return true;
}();

} // namespace vinput
