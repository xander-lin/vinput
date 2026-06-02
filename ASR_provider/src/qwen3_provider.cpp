#include "qwen3_provider.h"
#include <curl/curl.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace vinput {

static void writeWavHeader(FILE *f, int dataSize) {
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t subchunk1Size = 16;
    fwrite(&subchunk1Size, 4, 1, f);
    uint16_t audioFormat = 1;
    fwrite(&audioFormat, 2, 1, f);
    uint16_t numChannels = 1;
    fwrite(&numChannels, 2, 1, f);
    uint32_t sampleRate = 16000;
    fwrite(&sampleRate, 4, 1, f);
    uint32_t byteRate = 16000 * 2;
    fwrite(&byteRate, 4, 1, f);
    uint16_t blockAlign = 2;
    fwrite(&blockAlign, 2, 1, f);
    uint16_t bitsPerSample = 16;
    fwrite(&bitsPerSample, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&dataSize, 4, 1, f);
}

static size_t curlWriteCb(void *ptr, size_t size, size_t nmemb, void *user) {
    auto *s = static_cast<std::string *>(user);
    s->append(static_cast<char *>(ptr), size * nmemb);
    return size * nmemb;
}

Qwen3AsrProvider::Qwen3AsrProvider(const std::string &vllmSocket)
    : vllmSocket_(vllmSocket) {
    if (vllmSocket_.empty()) {
        const char *runtime = getenv("XDG_RUNTIME_DIR");
        auto base = runtime ? std::string(runtime) : "/tmp";
        vllmSocket_ = base + "/vllm-" + std::to_string(getuid()) + ".sock";
    }
    fprintf(stderr, "Vinput: vLLM socket = %s\n", vllmSocket_.c_str());
    tempWavPath_ = "/tmp/vinput_qwen3_" + std::to_string(getpid()) + ".wav";
}

Qwen3AsrProvider::~Qwen3AsrProvider() {
    stop();
    unlink(tempWavPath_.c_str());
}

void Qwen3AsrProvider::start() {
    if (recording_) return;
    recording_ = true;
    if (onState_) onState_(true);
    recordThread_ = std::thread([this]() { recordThread(); });
}

void Qwen3AsrProvider::stop() {
    if (!recording_) return;
    recording_ = false;
    if (recordThread_.joinable()) recordThread_.join();
    if (onState_) onState_(false);
    if (sendThread_.joinable()) sendThread_.join();
    sendThread_ = std::thread([this]() { sendToVllm(); });
}

void Qwen3AsrProvider::recordThread() {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "vinput", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput: PulseAudio error: %s\n", pa_strerror(error));
        recording_ = false;
        return;
    }

    FILE *f = fopen(tempWavPath_.c_str(), "wb");
    if (!f) {
        pa_simple_free(pa);
        recording_ = false;
        return;
    }

    fseek(f, 44, SEEK_SET);
    int totalBytes = 0;
    uint8_t buf[4096];

    while (recording_) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, "Vinput: read error: %s\n", pa_strerror(error));
            break;
        }
        fwrite(buf, 1, sizeof(buf), f);
        totalBytes += sizeof(buf);
    }

    rewind(f);
    writeWavHeader(f, totalBytes);
    fclose(f);
    pa_simple_free(pa);

    fprintf(stderr, "Vinput: recorded %d bytes to %s\n",
            totalBytes, tempWavPath_.c_str());
}

void Qwen3AsrProvider::sendToVllm() {
    FILE *f = fopen(tempWavPath_.c_str(), "rb");
    if (!f) {
        if (onError_) onError_("no audio file");
        return;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    rewind(f);
    if (fileSize <= 44) {
        fclose(f);
        if (onError_) onError_("empty recording");
        return;
    }

    std::vector<uint8_t> wavData(fileSize);
    fread(wavData.data(), 1, fileSize, f);
    fclose(f);

    // Base64 encode
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64;
    b64.reserve((fileSize + 2) / 3 * 4);
    for (long i = 0; i < fileSize; i += 3) {
        uint32_t n = (uint32_t(wavData[i]) << 16);
        if (i + 1 < fileSize) n |= (uint32_t(wavData[i + 1]) << 8);
        if (i + 2 < fileSize) n |= uint32_t(wavData[i + 2]);
        b64 += tbl[(n >> 18) & 63];
        b64 += tbl[(n >> 12) & 63];
        b64 += (i + 1 < fileSize) ? tbl[(n >> 6) & 63] : '=';
        b64 += (i + 2 < fileSize) ? tbl[n & 63] : '=';
    }

    std::string json = R"({"model":"Qwen/Qwen3-ASR-1.7B","messages":[)"
        R"({"role":"user","content":[{"type":"audio_url",)"
        R"("audio_url":{"url":"data:audio/wav;base64,)";
    json += b64;
    json += R"("}}]}]})";

    fprintf(stderr, "Vinput: sending %ld bytes audio to vLLM...\n", fileSize);

    CURL *curl = curl_easy_init();
    if (!curl) {
        if (onError_) onError_("curl init failed");
        return;
    }

    std::string resp;
    std::string url = "http://localhost/v1/chat/completions";

    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, vllmSocket_.c_str());
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        if (onError_) onError_(
            std::string("vLLM error: ") + curl_easy_strerror(res));
        return;
    }

    // Parse {"choices":[{"message":{"content":"..."}}]}
    auto p = resp.find("\"content\"");
    if (p == std::string::npos) {
        if (onError_) onError_("vLLM: no content");
        return;
    }
    p = resp.find('"', p + 10);
    if (p == std::string::npos) {
        if (onError_) onError_("vLLM: bad response");
        return;
    }
    p++;
    auto e = resp.find('"', p);
    if (e == std::string::npos) {
        if (onError_) onError_("vLLM: bad response");
        return;
    }

    std::string text = resp.substr(p, e - p);

    // Strip language tags like <|zh|>
    while (!text.empty() && text[0] == '<') {
        auto tagEnd = text.find("|>");
        if (tagEnd == std::string::npos) break;
        text = text.substr(tagEnd + 2);
    }

    fprintf(stderr, "Vinput: text = %s\n", text.c_str());
    if (onResult_) onResult_(text, true);
}

std::unique_ptr<IAsrProvider> Qwen3AsrProviderFactory::create() {
    return std::make_unique<Qwen3AsrProvider>();
}

static bool _qwen3Reg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<Qwen3AsrProviderFactory>());
    return true;
}();

} // namespace vinput
