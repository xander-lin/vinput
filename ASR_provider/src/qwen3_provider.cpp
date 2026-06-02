#include "qwen3_provider.h"
#include <curl/curl.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <sys/stat.h>
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

static std::string defaultUdsPath() {
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    auto base = runtime ? std::string(runtime) : "/tmp";
    return base + "/vllm-" + std::to_string(getuid()) + ".sock";
}

Qwen3AsrProvider::Qwen3AsrProvider() {
    tempWavPath_ = "/tmp/vinput_qwen3_" + std::to_string(getpid()) + ".wav";
}

Qwen3AsrProvider::~Qwen3AsrProvider() {
    recording_ = false;
    if (recordThread_.joinable()) recordThread_.join();
    if (sendThread_.joinable()) sendThread_.detach();
    unlink(tempWavPath_.c_str());
}

void Qwen3AsrProvider::setConfig(const std::string &key,
                                 const std::string &value) {
    if (key == "uds") {
        udsPath_ = value;
        if (!udsPath_.empty())
            fprintf(stderr, "Vinput: uds = %s\n", udsPath_.c_str());
    } else if (key == "host") {
        tcpHost_ = value;
    } else if (key == "port") {
        tcpPort_ = std::stoi(value);
        if (tcpPort_ > 0)
            fprintf(stderr, "Vinput: tcp = %s:%d\n",
                    tcpHost_.c_str(), tcpPort_);
    }
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

    // 按值捕获所有数据，detach 后 this 销毁也安全
    if (sendThread_.joinable()) sendThread_.join();
    auto wav = tempWavPath_;
    auto uds = udsPath_.empty() ? defaultUdsPath() : udsPath_;
    auto host = tcpHost_;
    auto port = tcpPort_;
    auto onR = onResult_;
    auto onE = onError_;
    sendThread_ = std::thread([wav, uds, host, port, onR, onE]() {
        sendToVllm(wav, uds, host, port, onR, onE);
    });
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

    fprintf(stderr, "Vinput: recorded %d bytes\n", totalBytes);
}

bool Qwen3AsrProvider::ensureServer(const std::string &uds) {
    struct stat st;
    if (stat(uds.c_str(), &st) == 0) return true;

    fprintf(stderr, "Vinput: starting vLLM service...\n");
    int ret = system("systemctl --user start vllm-qwen3 2>&1");
    fprintf(stderr, "Vinput: systemctl returned %d\n", ret);

    for (int i = 0; i < 60; i++) {
        usleep(500000);
        if (stat(uds.c_str(), &st) == 0) {
            fprintf(stderr, "Vinput: vLLM ready after %dms\n", (i + 1) * 500);
            return true;
        }
    }
    return false;
}

// 发送 HTTP 请求，返回是否成功
bool Qwen3AsrProvider::trySend(const std::string &url,
                               const std::string &udsPath,
                               const std::string &json,
                               std::string &resp) {
    CURL *curl = curl_easy_init();
    if (!curl) return false;

    struct curl_slist *hdrs = nullptr;
    hdrs = curl_slist_append(hdrs, "Content-Type: application/json");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)json.size());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, hdrs);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlWriteCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &resp);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    if (!udsPath.empty()) {
        curl_easy_setopt(curl, CURLOPT_UNIX_SOCKET_PATH, udsPath.c_str());
    }

    CURLcode res = curl_easy_perform(curl);
    curl_slist_free_all(hdrs);
    curl_easy_cleanup(curl);

    return res == CURLE_OK;
}

void Qwen3AsrProvider::sendToVllm(std::string wavPath, std::string uds,
                                  std::string host, int port,
                                  AsrResultCallback onResult,
                                  AsrErrorCallback onError) {
    FILE *f = fopen(wavPath.c_str(), "rb");
    if (!f) { if (onError) onError("no audio file"); return; }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    rewind(f);
    if (fileSize <= 44) { fclose(f); if (onError) onError("empty"); return; }

    std::vector<uint8_t> wavData(fileSize);
    fread(wavData.data(), 1, fileSize, f);
    fclose(f);

    // Base64
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

    std::string resp;
    bool ok = false;

    // 1) 尝试 UDS
    ensureServer(uds); // 懒加载
    fprintf(stderr, "Vinput: trying UDS %s\n", uds.c_str());
    ok = trySend("http://localhost/v1/chat/completions", uds, json, resp);
    if (!ok) {
        // 2) 回退 TCP
        if (port > 0) {
            auto tcpUrl = "http://" + host + ":" +
                          std::to_string(port) + "/v1/chat/completions";
            fprintf(stderr, "Vinput: UDS failed, trying TCP %s\n",
                    tcpUrl.c_str());
            ok = trySend(tcpUrl, "", json, resp);
        }
    }

    if (!ok) {
        if (onError) onError("vLLM: UDS and TCP both unreachable");
        return;
    }

    auto p = resp.find("\"content\"");
    if (p == std::string::npos) { if (onError) onError("vLLM: no content"); return; }
    p = resp.find('"', p + 10);
    if (p == std::string::npos) { if (onError) onError("vLLM: bad resp"); return; }
    p++;
    auto e = resp.find('"', p);
    if (e == std::string::npos) { if (onError) onError("vLLM: bad resp"); return; }

    std::string text = resp.substr(p, e - p);
    while (!text.empty() && text[0] == '<') {
        auto tagEnd = text.find("|>");
        if (tagEnd == std::string::npos) break;
        text = text.substr(tagEnd + 2);
    }

    fprintf(stderr, "Vinput: text = %s\n", text.c_str());
    if (onResult) onResult(text, true);
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
