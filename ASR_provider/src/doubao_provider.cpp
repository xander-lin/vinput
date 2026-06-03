#include "doubao_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <cstdint>
#include <arpa/inet.h>  // htonl

#include <curl/curl.h>

namespace vinput {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

static void writeWav(const std::vector<int16_t> &samples, const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) return;
    long ds = (long)samples.size() * 2;
    fwrite("RIFF",1,4,f);
    uint32_t cs=36+(uint32_t)ds; fwrite(&cs,4,1,f);
    fwrite("WAVE",1,4,f); fwrite("fmt ",1,4,f);
    uint32_t s1=16, sr=16000, br=32000;
    uint16_t ff=1, ch=1, bps=16, ba=2;
    fwrite(&s1,4,1,f);fwrite(&ff,2,1,f);fwrite(&ch,2,1,f);
    fwrite(&sr,4,1,f);fwrite(&br,4,1,f);fwrite(&ba,2,1,f);fwrite(&bps,2,1,f);
    fwrite("data",1,4,f); fwrite(&ds,4,1,f);
    fwrite(samples.data(),2,samples.size(),f);
    fclose(f);
}

// --- Doubao 二进制帧 ---
static std::vector<uint8_t> buildFrame(uint8_t msgType, uint8_t flags,
                                        const void *payload, uint32_t len) {
    std::vector<uint8_t> frame(8 + len);
    frame[0] = 0x11;  // version=1, header_size=4
    frame[1] = (uint8_t)((msgType << 4) | flags);
    frame[2] = (msgType == 0x01) ? 0x10 : 0x00;  // JSON for full req, raw for audio
    frame[3] = 0x00;
    uint32_t sz = htonl(len);
    memcpy(&frame[4], &sz, 4);
    if (len > 0) memcpy(&frame[8], payload, len);
    return frame;
}

// --- 简单 JSON 解析 (不依赖 external lib) ---
static std::string jsonGetString(const std::string &json, const char *key) {
    // 搜 \"key\"
    std::string pat = "\"" + std::string(key) + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return "";
    pos += pat.size();
    // 跳过 ": "
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    if (pos >= json.size() || json[pos] != '"') return "";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

static bool jsonGetBool(const std::string &json, const char *key) {
    std::string pat = "\"" + std::string(key) + "\"";
    auto pos = json.find(pat);
    if (pos == std::string::npos) return false;
    pos += pat.size();
    while (pos < json.size() && (json[pos] == ':' || json[pos] == ' ')) pos++;
    return pos < json.size() && (json[pos] == 't' || json[pos] == 'T');
}

DoubaoAsrProvider::DoubaoAsrProvider() {
    apiKey_ = "3a38c481-50bc-4cce-aaca-3cdf4e82c624";
    resourceId_ = "volc.bigasr.sauc.duration";

    // 常驻录音流
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;
    int error = 0;
    paStream_ = pa_simple_new(nullptr, "vinput-doubao-keep",
                              PA_STREAM_RECORD, nullptr, "voice",
                              &ss, nullptr, nullptr, &error);
    if (!paStream_) {
        fprintf(stderr, "Vinput Doubao: keep-alive PA error: %s\n", pa_strerror(error));
        return;
    }
    fprintf(stderr, "Vinput Doubao: keep-alive stream started\n");

    keepAliveThread_ = std::thread([this]() { keepAliveLoop(); });
}

DoubaoAsrProvider::~DoubaoAsrProvider() {
    keepAliveRunning_ = false;
    if (keepAliveThread_.joinable()) keepAliveThread_.join();
    if (paStream_) { pa_simple_free(paStream_); paStream_ = nullptr; }
    if (curlSend_) { curl_easy_cleanup((CURL *)curlSend_); curlSend_ = nullptr; }
    if (curlRecv_) { curl_easy_cleanup((CURL *)curlRecv_); curlRecv_ = nullptr; }
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKey_ = value;
    else if (key == "resource_id") resourceId_ = value;
}

void DoubaoAsrProvider::keepAliveLoop() {
    uint8_t buf[6400];
    int err = 0;
    while (keepAliveRunning_) {
        if (pa_simple_read(paStream_, buf, sizeof(buf), &err) < 0) {
            fprintf(stderr, "Vinput Doubao: keep-alive read error: %s\n",
                    pa_strerror(err));
            break;
        }
        if (sendRunning_) {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            auto *p = reinterpret_cast<int16_t *>(buf);
            samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
        }
    }
}

bool DoubaoAsrProvider::wsConnect() {
    if (curlSend_) { curl_easy_cleanup((CURL *)curlSend_); curlSend_ = nullptr; }
    if (curlRecv_) { curl_easy_cleanup((CURL *)curlRecv_); curlRecv_ = nullptr; }

    // 创建两个 handle: 一个发一个收
    CURL *cs = curl_easy_init();
    CURL *cr = curl_easy_init();
    if (!cs || !cr) {
        if (cs) curl_easy_cleanup(cs);
        if (cr) curl_easy_cleanup(cr);
        return false;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, ("X-Api-Key: " + apiKey_).c_str());
    headers = curl_slist_append(headers, ("X-Api-Resource-Id: " + resourceId_).c_str());
    headers = curl_slist_append(headers, "X-Api-Request-Id: vinput-doubao");
    headers = curl_slist_append(headers, "X-Api-Sequence: -1");

    auto setup = [&](CURL *c) {
        curl_easy_setopt(c, CURLOPT_URL,
                         "wss://openspeech.bytedance.com/api/v3/sauc/bigmodel");
        curl_easy_setopt(c, CURLOPT_CONNECT_ONLY, 2L);
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers);
        return curl_easy_perform(c);
    };

    CURLcode r1 = setup(cs);
    CURLcode r2 = setup(cr);
    curl_slist_free_all(headers);

    if (r1 != CURLE_OK) {
        fprintf(stderr, "Vinput Doubao: send WS connect failed: %s\n",
                curl_easy_strerror(r1));
        curl_easy_cleanup(cs); curl_easy_cleanup(cr);
        return false;
    }
    if (r2 != CURLE_OK) {
        fprintf(stderr, "Vinput Doubao: recv WS connect failed: %s\n",
                curl_easy_strerror(r2));
        curl_easy_cleanup(cs); curl_easy_cleanup(cr);
        return false;
    }

    curlSend_ = cs;
    curlRecv_ = cr;
    fprintf(stderr, "Vinput Doubao: WS connected (dual handle)\n");
    return true;
}

void DoubaoAsrProvider::wsSendFrame(uint8_t msgType, uint8_t flags,
                                     const void *payload, uint32_t len) {
    if (!curlSend_) return;
    auto frame = buildFrame(msgType, flags, payload, len);
    size_t sent = 0;
    CURLcode res = curl_ws_send((CURL *)curlSend_, frame.data(), frame.size(),
                                &sent, 0, CURLWS_RAW_MODE);
    if (res != CURLE_OK) {
        fprintf(stderr, "Vinput Doubao: ws send error: %s\n",
                curl_easy_strerror(res));
    }
}

bool DoubaoAsrProvider::wsRecvFrame(std::string &out) {
    if (!curlRecv_) return false;
    char buf[16384];
    size_t recv = 0;
    const struct curl_ws_frame *meta = nullptr;
    CURLcode res = curl_ws_recv((CURL *)curlRecv_, buf, sizeof(buf), &recv, &meta);
    if (res == CURLE_AGAIN) return false;
    if (res != CURLE_OK) {
        fprintf(stderr, "Vinput Doubao: ws recv error: %s\n",
                curl_easy_strerror(res));
        return false;
    }
    out.assign(buf, recv);
    return true;
}

bool DoubaoAsrProvider::parseResponse(const std::string &raw,
                                       std::string &text, bool &definite) {
    // raw 是二进制帧: header(4) + sequence(4) + payload_size(4) + payload
    if (raw.size() < 12) return false;
    uint8_t msgType = (raw[1] >> 4) & 0x0F;
    if (msgType == 0x0F) return false;  // error frame

    uint32_t payloadSz;
    memcpy(&payloadSz, &raw[8], 4);
    payloadSz = ntohl(payloadSz);
    if (raw.size() < 12 + payloadSz) return false;

    std::string json(raw.begin() + 12, raw.begin() + 12 + payloadSz);
    text = jsonGetString(json, "text");

    // 在 utterances 数组里找 "definite": true
    auto pos = json.find("\"utterances\"");
    if (pos != std::string::npos) {
        definite = json.find("\"definite\": true", pos) != std::string::npos ||
                   json.find("\"definite\":true", pos) != std::string::npos;
    }
    return true;
}

void DoubaoAsrProvider::start() {
    if (sendRunning_) return;

    // 连接 WebSocket
    if (!wsConnect()) {
        if (onError_) onError_("Doubao: WS connection failed");
        return;
    }

    // 发送 Full Client Request
    std::string req = "{"
        "\"audio\":{\"format\":\"pcm\",\"rate\":16000,\"bits\":16,\"channel\":1,"
        "\"language\":\"zh-CN\"},"
        "\"request\":{\"model_name\":\"bigmodel\",\"enable_itn\":true,"
        "\"enable_punc\":true}}";
    wsSendFrame(0x01, 0x00, req.data(), (uint32_t)req.size());

    // 读握手响应
    std::string resp;
    wsRecvFrame(resp);  // 忽略握手响应

    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    sendRunning_ = true;
    if (onState_) onState_(true);

    // 发送线程
    std::thread([this]() {
        int frameCount = 0;
        while (sendRunning_) {
            std::vector<int16_t> chunk;
            {
                std::lock_guard<std::mutex> lk(sampleMutex_);
                if (samples_.size() >= 3200) {
                    chunk.assign(samples_.begin(), samples_.begin() + 3200);
                    samples_.erase(samples_.begin(), samples_.begin() + 3200);
                }
            }
            if (chunk.size() >= 3200) {
                wsSendFrame(0x02, 0x00, chunk.data(), (uint32_t)(chunk.size() * 2));
                frameCount++;
                if (frameCount % 5 == 1)
                    fprintf(stderr, "Vinput Doubao: sent %d audio frames\n", frameCount);
            } else {
                usleep(20000);
            }
        }
        // 最后一包
        {
            wsSendFrame(0x02, 0x02, nullptr, 0);
            fprintf(stderr, "Vinput Doubao: sent final frame (total=%d)\n", frameCount);
        }
    }).detach();

    // 接收线程
    std::thread([this]() {
        std::string raw, text;
        bool definite = false;
        int recvCount = 0;
        while (sendRunning_ || !definite) {
            if (!wsRecvFrame(raw)) {
                usleep(50000);
                continue;
            }
            recvCount++;
            if (parseResponse(raw, text, definite)) {
                fprintf(stderr, "Vinput Doubao: recv #%d text=\"%s\" final=%d\n",
                        recvCount, text.c_str(), definite);
                if (!text.empty() && onResult_) onResult_(text, definite);
                if (definite) break;
            }
        }
        // 保存 WAV
        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            writeWav(samples_, tempWavPath_);
            fprintf(stderr, "Vinput Doubao: saved %zu samples to %s\n",
                    samples_.size(), tempWavPath_.c_str());
        }
        if (onState_) onState_(false);
    }).detach();
}

void DoubaoAsrProvider::stop() {
    sendRunning_ = false;
}

std::unique_ptr<IAsrProvider> DoubaoAsrProviderFactory::create() {
    return std::make_unique<DoubaoAsrProvider>();
}

static bool _doubaoReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<DoubaoAsrProviderFactory>());
    return true;
}();

} // namespace vinput
