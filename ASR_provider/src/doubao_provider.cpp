#include "doubao_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <curl/curl.h>
#include <unistd.h>
#include <sys/random.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <fstream>
#include <memory>
#include <ebur128.h>

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

static bool writeWav(std::vector<int16_t> &samples, const std::string &path) {
    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    constexpr double kTargetLUFS = -16.0;
    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Vinput Doubao: audio too quiet (%.1f LUFS), skip norm\n", loudness);
        gain = 1.0;
    } else {
        gain = std::pow(10.0, (kTargetLUFS - loudness) / 20.0);
    }
    fprintf(stderr, "Vinput Doubao: loudness %.1f LUFS, gain=%.2f\n", loudness, gain);

    for (auto &s : samples) {
        int32_t v = static_cast<int32_t>(s) * gain;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s = static_cast<int16_t>(v);
    }

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Vinput Doubao: cannot write WAV to %s\n", path.c_str());
        return false;
    }

    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = 16000, br = 32000;
    uint16_t fmtTag = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmtTag, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize;
    fwrite(&ds, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);
    return true;
}

DoubaoAsrProvider::DoubaoAsrProvider() {
    loadConfig(apiKey_, resourceId_);
}

DoubaoAsrProvider::~DoubaoAsrProvider() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
    else if (key == "resource_id") resourceId_ = value;
}

void DoubaoAsrProvider::recordLoop() {
    using Clock = std::chrono::steady_clock;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    pa_simple *pa = pa_simple_new(nullptr, "vinput-doubao", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput Doubao: PA error: %s\n", pa_strerror(error));
        if (onError_) onError_("Doubao: microphone open failed");
        return;
    }

    // --- 录音阶段 ---
    uint8_t buf[4096];
    while (!stopRequested_) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf);
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
    }

    if (onState_) onState_(false);

    // --- Drain 阶段: 等待下一个 burst 以捕获尾部音频 ---
    // CX31993 芯片有 ~2s burst / ~2s buffering 的硬件占空比。
    // 计数两次长阻塞 (>500ms) 确保跨过一个完整的 burst 周期。
    auto drainStart = Clock::now();
    auto lastRead = drainStart;
    int longBlockCount = 0;
    constexpr double kMaxDrainMs = 4000;

    while (true) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf);
        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
        }

        auto now = Clock::now();
        double readMs = std::chrono::duration<double, std::milli>(now - lastRead).count();
        lastRead = now;

        if (readMs > 500) longBlockCount++;

        double drainMs = std::chrono::duration<double, std::milli>(now - drainStart).count();
        if (longBlockCount >= 2 || drainMs >= kMaxDrainMs) break;
    }

    pa_simple_free(pa);

    // --- 处理 ---
    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        batch.swap(samples_);
    }

    auto wav = sessionWav_;
    auto onR = sessionOnR_;
    auto onE = sessionOnE_;

    if (!batch.empty()) {
        processRecording(std::move(batch), wav, onR, onE);
    }
}

void DoubaoAsrProvider::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    sessionWav_ = "/tmp/vinput_doubao_" + std::to_string(getpid()) + "_"
                  + std::to_string(time(nullptr)) + ".wav";
    sessionOnR_ = onResult_;
    sessionOnE_ = onError_;
    if (onState_) onState_(true);
    recordThread_ = std::thread(&DoubaoAsrProvider::recordLoop, this);
}

void DoubaoAsrProvider::stop() {
    stopRequested_ = true;
    if (onState_) onState_(false);
}

void DoubaoAsrProvider::processRecording(std::vector<int16_t> samples,
                                           const std::string &wavPath,
                                           AsrResultCallback onR,
                                           AsrErrorCallback onE) {
    if (!writeWav(samples, wavPath)) {
        if (onE) onE("Doubao: failed to write WAV");
        return;
    }
    fprintf(stderr, "Vinput Doubao: recorded %zu samples to %s\n",
            samples.size(), wavPath.c_str());

    auto apiKey = apiKey_;
    auto resourceId = resourceId_;
    if (apiKey.empty() || resourceId.empty()) {
        if (onE) onE("Doubao: missing api_key or resource_id in ~/.config/vinput/doubao.json");
        return;
    }

    std::thread([=]() {
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

        std::string taskId = generateUuid();

        {
            CURL *curl = curl_easy_init();
            if (!curl) {
                if (onE) onE("Doubao: curl init failed");
                return;
            }
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
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

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

        constexpr int kMaxPoll = 75;
        for (int pollCount = 1; pollCount <= kMaxPoll; pollCount++) {
            usleep(800000);

            CURL *curl = curl_easy_init();
            if (!curl) {
                if (onE) onE("Doubao: curl init failed");
                return;
            }
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
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

            CURLcode res = curl_easy_perform(curl);
            long httpCode = 0;
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
            curl_slist_free_all(headers);
            curl_easy_cleanup(curl);

            if (res != CURLE_OK || httpCode != 200) {
                fprintf(stderr, "Vinput Doubao: query #%d HTTP %ld error, retry\n",
                        pollCount, httpCode);
                continue;
            }

            std::string statusCode = getHeader(respHdr, "x-api-status-code");

            if (statusCode == "20000000") {
                std::string text = jsonGetString(respBody, "text");
                fprintf(stderr, "Vinput Doubao: result=\"%s\" (polled %d)\n",
                        text.c_str(), pollCount);
                if (onR) onR(text, true);
                return;
            }
            if (statusCode == "20000003") {
                fprintf(stderr, "Vinput Doubao: no speech detected\n");
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
