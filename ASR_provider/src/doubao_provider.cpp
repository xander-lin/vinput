#include "doubao_provider.h"
#include "vinput_config.h"

#include <curl/curl.h>
#include <unistd.h>
#include <sys/random.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
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

static std::string generateUuid() {
    uint8_t r[16];
    if (getrandom(r, sizeof(r), 0) != (ssize_t)sizeof(r)) {
        for (size_t i = 0; i < sizeof(r); i++)
            r[i] = (uint8_t)(rand() ^ (time(nullptr) * (i + 1)));
    }
    r[6] = (r[6] & 0x0F) | 0x40;
    r[8] = (r[8] & 0x3F) | 0x80;
    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             r[0],r[1],r[2],r[3],r[4],r[5],r[6],r[7],
             r[8],r[9],r[10],r[11],r[12],r[13],r[14],r[15]);
    return buf;
}

static size_t writeCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static size_t headerCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

static std::string getHeader(const std::string &headers, const std::string &name) {
    std::string pattern = "\n" + name + ":";
    auto pos = headers.find(pattern);
    if (pos == std::string::npos) {
        if (headers.size() >= name.size() + 1 &&
            headers.substr(0, name.size()) == name && headers[name.size()] == ':') {
            pos = 0;
        } else {
            return "";
        }
    } else {
        pos++;
    }
    pos += name.size() + 1;
    while (pos < headers.size() && headers[pos] == ' ') pos++;
    std::string value;
    while (pos < headers.size() && headers[pos] != '\r' && headers[pos] != '\n') {
        if (headers[pos] != ' ') value += headers[pos];
        pos++;
    }
    return value;
}

static void loadConfig(std::string &apiKey, std::string &resourceId) {
    const char *home = getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/vinput/doubao.json";
    std::ifstream f(path);
    if (!f) {
        fprintf(stderr, "Vinput Doubao: no config at %s\n", path.c_str());
        return;
    }
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    apiKey = jsonGetString(json, "api_key");
    resourceId = jsonGetString(json, "resource_id");
}

DoubaoAsrProvider::DoubaoAsrProvider() {
    loadConfig(apiKey_, resourceId_);
    auto adv = advancedSection("doubao");
    if (!adv.empty()) {
        pollIntervalMsec_ = jsonInt(adv, "poll_interval_msec", pollIntervalMsec_);
        maxPolls_ = jsonInt(adv, "max_polls", maxPolls_);
        submitTimeout_ = (long)jsonInt(adv, "submit_timeout_sec", (int)submitTimeout_);
        queryTimeout_ = (long)jsonInt(adv, "query_timeout_sec", (int)queryTimeout_);
    }
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
    else if (key == "resource_id") resourceId_ = value;
}

void DoubaoAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    processRecording(std::move(samples), wavPath, onResult_, onError_);
}

void DoubaoAsrProvider::processRecording(std::vector<int16_t> samples,
                                           const std::string &wavPath,
                                           AsrResultCallback onR,
                                           AsrErrorCallback onE) {
    fprintf(stderr, "Vinput Doubao: recorded %zu samples to %s\n",
            samples.size(), wavPath.c_str());

    auto apiKey = apiKey_;
    auto resourceId = resourceId_;
    if (apiKey.empty() || resourceId.empty()) {
        if (onE) onE("Doubao: missing api_key or resource_id in ~/.config/vinput/doubao.json");
        return;
    }

    std::thread([=, this]() {
        auto t0 = std::chrono::steady_clock::now();
        struct Cleanup { std::string p; ~Cleanup() { unlink(p.c_str()); } } _wav{wavPath};

        std::ifstream wf(wavPath, std::ios::binary);
        if (!wf) {
            if (onE) onE("Doubao: failed to read WAV");
            return;
        }
        std::vector<uint8_t> wavData((std::istreambuf_iterator<char>(wf)),
                                      std::istreambuf_iterator<char>());
        if (wavData.empty()) {
            if (onE) onE("Doubao: empty WAV file");
            return;
        }
        std::string b64 = base64Encode(wavData.data(), wavData.size());
        auto tEncode = std::chrono::steady_clock::now();

        std::string taskId = generateUuid();

        {
            CURL *curl = getCurl();
            if (!curl) {
                if (onE) onE("Doubao: curl init failed");
                return;
            }
            curl_easy_reset(curl);
            std::string respBody, respHdr;

            std::string submitBody =
                "{"
                "\"audio\":{"
                "\"format\":\"wav\","
                "\"data\":\"" + b64 + "\""
                "},"
                "\"request\":{"
                "\"model_name\":\"bigmodel\","
                "\"enable_itn\":true,"
                "\"enable_punc\":true"
                "}"
                "}";

            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers,
                ("X-Api-Key: " + apiKey).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Resource-Id: " + resourceId).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Request-Id: " + taskId).c_str());
            headers = curl_slist_append(headers, "X-Api-Sequence: -1");

            curl_easy_setopt(curl, CURLOPT_URL,
                "https://openspeech.bytedance.com/api/v3/auc/bigmodel/submit");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, submitBody.c_str());
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respHdr);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, submitTimeout_);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);

            fprintf(stderr, "Vinput Doubao: submit HTTP %ld\n", httpCode);
            if (res != CURLE_OK || httpCode != 200) {
                fprintf(stderr, "Vinput Doubao: submit failed, body=%s\n", respBody.c_str());
                if (onE) onE("Doubao: submit failed (HTTP " + std::to_string(httpCode) + ")");
                return;
            }

            std::string statusCode = getHeader(respHdr, "x-api-status-code");
            if (!statusCode.empty() && statusCode != "20000000") {
                fprintf(stderr, "Vinput Doubao: submit rejected status=%s\n", statusCode.c_str());
                if (onE) onE("Doubao: submit rejected (" + statusCode + ")");
                return;
            }
        }

        auto tSubmit = std::chrono::steady_clock::now();

        for (int pollCount = 1; pollCount <= maxPolls_; pollCount++) {
            usleep(pollIntervalMsec_ * 1000);

            CURL *curl = getCurl();
            if (!curl) {
                if (onE) onE("Doubao: curl init failed");
                return;
            }
            curl_easy_reset(curl);
            std::string respBody, respHdr;

            struct curl_slist *headers = nullptr;
            headers = curl_slist_append(headers, "Content-Type: application/json");
            headers = curl_slist_append(headers,
                ("X-Api-Key: " + apiKey).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Resource-Id: " + resourceId).c_str());
            headers = curl_slist_append(headers,
                ("X-Api-Request-Id: " + taskId).c_str());

            curl_easy_setopt(curl, CURLOPT_URL,
                "https://openspeech.bytedance.com/api/v3/auc/bigmodel/query");
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "{}");
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, headerCb);
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &respHdr);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, queryTimeout_);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);

            if (res != CURLE_OK || httpCode != 200) {
                fprintf(stderr, "Vinput Doubao: query #%d HTTP %ld error, retry\n",
                        pollCount, httpCode);
                continue;
            }

            std::string statusCode = getHeader(respHdr, "x-api-status-code");

            if (statusCode == "20000000") {
                auto tResult = std::chrono::steady_clock::now();
                std::string text = jsonGetString(respBody, "text");
                fprintf(stderr, "Vinput Doubao [timer] encode=%ldms submit=%ldms poll=%ldms poll_n=%d text=\"%s\"\n",
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tSubmit - tEncode).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tSubmit).count(),
                        pollCount, text.c_str());
                if (onR) onR(text, true);
                return;
            }
            if (statusCode == "20000003") {
                auto tResult = std::chrono::steady_clock::now();
                fprintf(stderr, "Vinput Doubao [timer] encode=%ldms submit=%ldms poll=%ldms poll_n=%d (silence)\n",
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tSubmit - tEncode).count(),
                        (long)std::chrono::duration_cast<std::chrono::milliseconds>(tResult - tSubmit).count(),
                        pollCount);
                if (onR) onR("", true);
                return;
            }
            if (statusCode != "20000001" && statusCode != "20000002") {
                fprintf(stderr, "Vinput Doubao: query status=%s body=%s\n",
                        statusCode.c_str(), respBody.c_str());
                if (onE) onE("Doubao: recognition failed (" + statusCode + ")");
                return;
            }
            fprintf(stderr, "Vinput Doubao: query #%d processing...\n", pollCount);
        }

        fprintf(stderr, "Vinput Doubao: query timeout\n");
        if (onE) onE("Doubao: query timeout (60s)");
    }).detach();
}

std::unique_ptr<IAsrProvider> DoubaoAsrProviderFactory::create() {
    return std::make_unique<DoubaoAsrProvider>();
}

static struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlInit() { curl_global_cleanup(); }
} _curlInit;

static bool _doubaoReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<DoubaoAsrProviderFactory>());
    return true;
}();

} // namespace vinput
