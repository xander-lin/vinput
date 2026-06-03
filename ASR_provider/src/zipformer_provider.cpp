#include "zipformer_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
#include <chrono>
#include <ebur128.h>
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
    : modelDir_("~/.local/share/vinput/models/zipformer-zh-en") {}

ZipformerAsrProvider::~ZipformerAsrProvider() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void ZipformerAsrProvider::setConfig(const std::string &key,
                                      const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void ZipformerAsrProvider::recordLoop() {


    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto t0 = std::chrono::steady_clock::now();
    pa_simple *pa = pa_simple_new(nullptr, "vinput-zip", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput: PA error: %s\n", pa_strerror(error));
        if (onError_) onError_("Zipformer: microphone open failed");
        return;
    }
    auto tPaOpen = std::chrono::steady_clock::now();

    // 64K 缓冲区 ≈ 2s 音频, 一次读跨过一个硬件周期, 无需 drain
    uint8_t buf[65536];
    int nReads = 0;
    while (!stopRequested_) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf);
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
        nReads++;
    }
    auto tRecordEnd = std::chrono::steady_clock::now();

    if (onState_) onState_(false);

    pa_simple_free(pa);
    auto tPaClose = std::chrono::steady_clock::now();

    fprintf(stderr, "Vinput Zipformer [timer] pa_open=%ldms record=%ldms pa_close=%ldms n_reads=%d samples=%zu\n",
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
    auto dir = sessionDir_;
    auto onR = sessionOnR_;
    auto onE = sessionOnE_;

    if (!batch.empty()) {
        processRecording(std::move(batch), wav, dir, onR, onE);
    }
}

void ZipformerAsrProvider::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    sessionWav_ = "/tmp/vinput_zip_" + std::to_string(getpid()) + "_"
                  + std::to_string(time(nullptr)) + ".wav";
    sessionDir_ = expandPath(modelDir_);
    sessionOnR_ = onResult_;
    sessionOnE_ = onError_;
    if (onState_) onState_(true);

    recordThread_ = std::thread(&ZipformerAsrProvider::recordLoop, this);
}

void ZipformerAsrProvider::stop() {
    stopRequested_ = true;
    if (onState_) onState_(false);
}

void ZipformerAsrProvider::processRecording(std::vector<int16_t> samples,
                                              const std::string &wavPath,
                                              const std::string &dir,
                                              AsrResultCallback onR,
                                              AsrErrorCallback onE) {
    normalizeAndWriteWav(samples, wavPath);
    runTranscribe(wavPath, dir, onR, onE);
}

void ZipformerAsrProvider::normalizeAndWriteWav(std::vector<int16_t> &samples,
                                                 const std::string &wavPath) {
    auto t0 = std::chrono::steady_clock::now();

    int16_t maxAbs = 0;
    for (auto s : samples) {
        int16_t a = s >= 0 ? s : (int16_t)-s;
        if (a > maxAbs) maxAbs = a;
    }

    auto *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    constexpr double kTargetLUFS = -16.0;
    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Vinput: audio too quiet (%.1f LUFS), skip normalization\n", loudness);
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

    FILE *f = fopen(wavPath.c_str(), "wb");
    if (!f) return;

    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize;
    fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = 16000, br = 32000;
    uint16_t fmt_tag = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmt_tag, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize;
    fwrite(&ds, 4, 1, f);

    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);

    auto tWav = std::chrono::steady_clock::now();

    fprintf(stderr, "Vinput Zipformer [timer] ebur128=%ldms gain=%ldms wav_write=%ldms loudness=%.1f gain=%.2f max_abs=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - tGain).count(),
            loudness, gain, (int)maxAbs, samples.size());
}

void ZipformerAsrProvider::runTranscribe(const std::string &wav,
                                          const std::string &dir,
                                          AsrResultCallback onR,
                                          AsrErrorCallback onE) {
    std::thread([=]() {
        auto t0 = std::chrono::steady_clock::now();

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            if (onE) onE("Zipformer: pipe failed");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]); close(pipefd[1]);
            if (onE) onE("Zipformer: fork failed");
            return;
        }
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDERR_FILENO);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);

            auto sherpaBin = expandPath("~/.local/share/vinput/sherpa-onnx/bin/sherpa-onnx");
            std::string encoder = "--encoder=" + dir + "/encoder-epoch-99-avg-1.onnx";
            std::string decoder = "--decoder=" + dir + "/decoder-epoch-99-avg-1.onnx";
            std::string joiner  = "--joiner="  + dir + "/joiner-epoch-99-avg-1.onnx";
            std::string tokens  = "--tokens="  + dir + "/tokens.txt";

            execl(sherpaBin.c_str(), sherpaBin.c_str(),
                  encoder.c_str(),
                  decoder.c_str(),
                  joiner.c_str(),
                  tokens.c_str(),
                  "--provider=cpu",
                  "--num-threads=30",
                  wav.c_str(),
                  nullptr);
            _exit(127);
        }

        close(pipefd[1]);

        std::string output;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output.append(buf, (size_t)n);
        }
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);
        auto tRecv = std::chrono::steady_clock::now();

        auto pos = output.rfind("\"text\"");
        std::string text;
        if (pos != std::string::npos) {
            pos += 9;
            auto end = output.find('"', pos);
            if (end != std::string::npos)
                text = output.substr(pos, end - pos);
        }

        auto tParse = std::chrono::steady_clock::now();
        fprintf(stderr, "Vinput Zipformer [timer] exec_total=%ldms parse=%ldms text=\"%s\"\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.c_str());

        unlink(wav.c_str());
        if (onR && !text.empty()) onR(text, true);
        else if (onE) onE("Zipformer: empty result");
    }).detach();
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
