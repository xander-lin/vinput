#include "qwen_provider.h"
#include "buffer_detect.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <curl/curl.h>
#include <unistd.h>
#include <sys/random.h>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <cmath>
#include <chrono>
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

static bool writeWav(std::vector<int16_t> &samples, const std::string &path) {
    auto t0 = std::chrono::steady_clock::now();

    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    constexpr double kTargetLUFS = -16.0;
    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Vinput Qwen: audio too quiet (%.1f LUFS), skip norm\n", loudness);
        gain = 1.0;
    } else {
        gain = std::pow(10.0, (kTargetLUFS - loudness) / 20.0);
    }

    auto tEbur = std::chrono::steady_clock::now();

    for (auto &s : samples) {
        int32_t v = static_cast<int32_t>(s) * gain;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s = static_cast<int16_t>(v);
    }

    auto tGain = std::chrono::steady_clock::now();

    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        fprintf(stderr, "Vinput Qwen: cannot write WAV to %s\n", path.c_str());
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

    auto tWav = std::chrono::steady_clock::now();

    fprintf(stderr, "Vinput Qwen [timer] ebur128=%ldms gain=%ldms wav_write=%ldms loudness=%.1f gain=%.2f samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - tGain).count(),
            loudness, gain, samples.size());
    return true;
}

QwenAsrProvider::QwenAsrProvider() {
    loadConfig(apiKey_);
}

QwenAsrProvider::~QwenAsrProvider() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void QwenAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
}

void QwenAsrProvider::recordLoop() {

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto t0 = std::chrono::steady_clock::now();
    pa_simple *pa = pa_simple_new(nullptr, "vinput-qwen", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput Qwen: PA error: %s\n", pa_strerror(error));
        if (onError_) onError_("Qwen: microphone open failed");
        return;
    }
    auto tPaOpen = std::chrono::steady_clock::now();

    if (bufferBytes_ == 0) {
        bufferBytes_ = loadOrDetectBufferBytes([this](const std::string &msg) {
            if (onStatusText_) onStatusText_(msg);
        });
    }
    std::vector<uint8_t> buf(bufferBytes_);
    size_t kFrameCount = buf.size() / 2;
    int nReads = 0;
    while (!stopRequested_) {
        if (pa_simple_read(pa, buf.data(), buf.size(), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf.data());
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.insert(samples_.end(), p, p + kFrameCount);
        nReads++;
    }
    auto tRecordEnd = std::chrono::steady_clock::now();

    if (onState_) onState_(false);

    pa_simple_free(pa);
    auto tPaClose = std::chrono::steady_clock::now();

    fprintf(stderr, "Vinput Qwen [timer] pa_open=%ldms record=%ldms pa_close=%ldms n_reads=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaOpen - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecordEnd - tPaOpen).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaClose - tRecordEnd).count(),
              nReads, samples_.size());

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

void QwenAsrProvider::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    sessionWav_ = "/tmp/vinput_qwen_" + std::to_string(getpid()) + "_"
                  + std::to_string(time(nullptr)) + ".wav";
    sessionOnR_ = onResult_;
    sessionOnE_ = onError_;
    if (onState_) onState_(true);
    recordThread_ = std::thread(&QwenAsrProvider::recordLoop, this);
}

void QwenAsrProvider::stop() {
    stopRequested_ = true;
    if (onState_) onState_(false);
}

void QwenAsrProvider::processRecording(std::vector<int16_t> samples,
                                         const std::string &wavPath,
                                         AsrResultCallback onR,
                                         AsrErrorCallback onE) {
    if (!writeWav(samples, wavPath)) {
        if (onE) onE("Qwen: failed to write WAV");
        return;
    }
    fprintf(stderr, "Vinput Qwen: recorded %zu samples to %s\n",
            samples.size(), wavPath.c_str());

    auto apiKey = apiKey_;
    if (apiKey.empty()) {
        if (onE) onE("Qwen: missing api_key in ~/.config/vinput/qwen.json");
        return;
    }

    std::thread([=]() {
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

        CURL *curl = curl_easy_init();
        if (!curl) {
            if (onE) onE("Qwen: curl init failed");
            return;
        }
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
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

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
