#include "zipformer_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
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
    if (key == "model_dir") modelDir_ = value;
}

void ZipformerAsrProvider::recordLoop() {
    using Clock = std::chrono::steady_clock;

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    pa_simple *pa = pa_simple_new(nullptr, "vinput-zip", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput: PA error: %s\n", pa_strerror(error));
        if (onError_) onError_("Zipformer: microphone open failed");
        return;
    }

    // --- 录音阶段 ---
    uint8_t buf[4096];
    recordStart_ = Clock::now();
    while (!stopRequested_) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf);
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
    }

    if (onState_) onState_(false);

    // --- Drain: wait (2000ms - elapsed%2000) + 100ms ---
    // CX31993 2s burst/buffer cycle: wait until next block boundary, capture tail burst
    auto stopTime = Clock::now();
    using Ms = std::chrono::milliseconds;
    auto elapsed = std::chrono::duration_cast<Ms>(stopTime - recordStart_).count();
    auto waitMs = 2100 - (elapsed % 2000);
    auto drainEnd = stopTime + Ms(waitMs);
    fprintf(stderr, "Vinput Zipformer: recorded %ldms, wait %ldms (elapsed%%2000=%ld)\n",
            elapsed, waitMs, elapsed % 2000);

    while (Clock::now() < drainEnd) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) break;
        auto *p = reinterpret_cast<int16_t *>(buf);
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.insert(samples_.end(), p, p + sizeof(buf) / 2);
    }

    pa_simple_free(pa);

    // --- 处理 ---
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
    int16_t maxAbs = 0;
    for (auto s : samples) {
        int16_t a = s >= 0 ? s : (int16_t)-s;
        if (a > maxAbs) maxAbs = a;
    }
    fprintf(stderr, "Vinput: %zu samples, max_abs=%d\n",
            samples.size(), (int)maxAbs);

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
    fprintf(stderr, "Vinput: measured loudness %.1f LUFS, gain=%.2f\n", loudness, gain);

    for (auto &s : samples) {
        int32_t v = static_cast<int32_t>(s) * gain;
        if (v > 32767) v = 32767;
        if (v < -32768) v = -32768;
        s = static_cast<int16_t>(v);
    }

    FILE *f = fopen(wavPath.c_str(), "wb");
    if (!f) return;

    long dataSize = (long)samples.size() * 2;
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

    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);

    fprintf(stderr, "Vinput: Zipformer recorded %ld samples (%ld bytes)\n",
            (long)samples.size(), dataSize);
}

void ZipformerAsrProvider::runTranscribe(const std::string &wav,
                                          const std::string &dir,
                                          AsrResultCallback onR,
                                          AsrErrorCallback onE) {
    auto sherpaBin = expandPath("~/.local/share/vinput/sherpa-onnx/bin/sherpa-onnx");
    auto encoder  = dir + "/encoder-epoch-99-avg-1.onnx";
    auto decoder  = dir + "/decoder-epoch-99-avg-1.onnx";
    auto joiner   = dir + "/joiner-epoch-99-avg-1.onnx";
    auto tokens   = dir + "/tokens.txt";

    std::thread([=]() {
        int outPipe[2], errPipe[2];
        if (pipe(outPipe) < 0 || pipe(errPipe) < 0) {
            if (onE) onE("Zipformer: pipe failed");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            if (onE) onE("Zipformer: fork failed");
            close(outPipe[0]); close(outPipe[1]);
            close(errPipe[0]); close(errPipe[1]);
            return;
        }

        if (pid == 0) {
            dup2(outPipe[1], STDOUT_FILENO);
            dup2(errPipe[1], STDERR_FILENO);
            close(outPipe[0]); close(outPipe[1]);
            close(errPipe[0]); close(errPipe[1]);

            std::string encArg = "--encoder=" + encoder;
            std::string decArg = "--decoder=" + decoder;
            std::string joiArg = "--joiner="  + joiner;
            std::string tokArg = "--tokens="  + tokens;

            execl(sherpaBin.c_str(), sherpaBin.c_str(),
                  encArg.c_str(), decArg.c_str(), joiArg.c_str(), tokArg.c_str(),
                  "--provider=cpu", "--num-threads=4",
                  wav.c_str(), nullptr);
            _exit(127);
        }

        close(outPipe[1]);
        close(errPipe[1]);

        auto readFd = [](int fd, std::string &out) {
            char buf[4096];
            ssize_t n;
            while ((n = read(fd, buf, sizeof(buf))) > 0)
                out.append(buf, (size_t)n);
            close(fd);
        };

        std::string stdoutText, stderrText;
        std::thread outReader(readFd, outPipe[0], std::ref(stdoutText));
        readFd(errPipe[0], std::ref(stderrText));
        outReader.join();

        int status;
        waitpid(pid, &status, 0);

        if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Vinput: sherpa-onnx exited %d, stderr:\n%s\n",
                    WEXITSTATUS(status), stderrText.c_str());
            if (onE) onE("Zipformer: recognition failed");
            return;
        }

        auto pos = stderrText.rfind("\"text\"");
        if (pos == std::string::npos) {
            if (onE) onE("Zipformer: no text in output");
            return;
        }

        pos += 9;
        auto end = stderrText.find('"', pos);
        std::string text = stderrText.substr(pos, end - pos);

        fprintf(stderr, "Vinput: text = %s\n", text.c_str());
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
