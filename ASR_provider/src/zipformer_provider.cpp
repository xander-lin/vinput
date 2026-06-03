#include "zipformer_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace vinput {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

ZipformerAsrProvider::ZipformerAsrProvider()
    : modelDir_("~/.local/share/vinput/models/zipformer-zh-en") {
    tempWavPath_ = "/tmp/vinput_zip_" + std::to_string(getpid()) + ".wav";
}

ZipformerAsrProvider::~ZipformerAsrProvider() {
    recording_ = false;
    if (recordThread_.joinable()) recordThread_.join();
    unlink(tempWavPath_.c_str());
}

void ZipformerAsrProvider::setConfig(const std::string &key,
                                      const std::string &value) {
    if (key == "model_dir") modelDir_ = value;
}

void ZipformerAsrProvider::start() {
    if (recording_) return;
    recording_ = true;
    if (onState_) onState_(true);
    recordThread_ = std::thread([this]() { recordThread(); });
}

void ZipformerAsrProvider::stop() {
    if (!recording_) return;
    recording_ = false;
    if (recordThread_.joinable()) recordThread_.join();
    if (onState_) onState_(false);

    // 后台子进程识别
    auto wav = tempWavPath_;
    auto onR = onResult_;
    auto onE = onError_;
    std::thread([wav, onR, onE, this]() {
        auto dir = expandPath(modelDir_);
        auto sherpaBin = expandPath("~/.local/share/vinput/sherpa-onnx/bin/sherpa-onnx");
        auto cmd = sherpaBin
                   + " --encoder=" + dir + "/encoder-epoch-99-avg-1.onnx"
                   + " --decoder=" + dir + "/decoder-epoch-99-avg-1.onnx"
                   + " --joiner=" + dir + "/joiner-epoch-99-avg-1.onnx"
                   + " --tokens=" + dir + "/tokens.txt"
                   + " --provider=cpu --num-threads=4"
                   + " " + wav + " 2>/dev/null";

        fprintf(stderr, "Vinput: running sherpa-onnx subprocess...\n");
        auto *pipe = popen(cmd.c_str(), "r");
        if (!pipe) { if (onE) onE("Zipformer: popen failed"); return; }

        std::string output;
        char buf[4096];
        while (fgets(buf, sizeof(buf), pipe)) output += buf;
        int rc = pclose(pipe);

        if (rc != 0) {
            fprintf(stderr, "Vinput: sherpa-onnx exited %d\n", rc);
            if (onE) onE("Zipformer: recognition failed");
            return;
        }

        // 解析文本: 从 JSON 取 "text" 字段
        auto pos = output.rfind("\"text\"");
        if (pos == std::string::npos) {
            // 尝试取最后一行非 JSON 的文本
            pos = output.find_last_of('\n', output.size() - 2);
            std::string text;
            if (pos != std::string::npos) {
                text = output.substr(0, pos);
                pos = text.find_last_of('\n');
                if (pos != std::string::npos) text = text.substr(pos + 1);
            }
            text = output;
            // 去掉首尾空白
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                text.pop_back();
            while (!text.empty() && (text[0] == '\n' || text[0] == ' '))
                text.erase(0, 1);

            if (!text.empty()) {
                fprintf(stderr, "Vinput: text = %s\n", text.c_str());
                if (onR) onR(text, true);
            } else {
                if (onE) onE("Zipformer: empty output");
            }
            return;
        }

        pos += 8; // skip "text": "
        auto end = output.find('"', pos);
        std::string text = output.substr(pos, end - pos);

        fprintf(stderr, "Vinput: text = %s\n", text.c_str());
        if (onR && !text.empty()) onR(text, true);
        else if (onE) onE("Zipformer: empty result");
    }).detach();
}

void ZipformerAsrProvider::recordThread() {
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
    if (!f) { pa_simple_free(pa); recording_ = false; return; }
    fseek(f, 44, SEEK_SET);

    uint8_t buf[4096];
    while (recording_) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        fwrite(buf, 1, sizeof(buf), f);
    }

    long dataSize = ftell(f) - 44;
    rewind(f);
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = 16000, br = 32000;
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize;
    fwrite(&ds, 4, 1, f);

    fclose(f);
    pa_simple_free(pa);
    fprintf(stderr, "Vinput: Zipformer recorded %ld bytes\n", dataSize);
}

std::unique_ptr<IAsrProvider> ZipformerAsrProviderFactory::create() {
    return std::make_unique<ZipformerAsrProvider>();
}

static bool _zipReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<ZipformerAsrProviderFactory>());
    return true;
}();

} // namespace vinput
