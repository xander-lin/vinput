#include "audio_capture.h"
#include "buffer_detect.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <ctime>
#include <chrono>
#include <ebur128.h>

namespace vinput {

AudioCapture::AudioCapture() {}

AudioCapture::~AudioCapture() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void AudioCapture::start() {
    if (recordThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        samples_.clear();
    }
    stopRequested_ = false;
    wavPath_ = "/tmp/vinput_cap_" + std::to_string(getpid()) + "_"
               + std::to_string(time(nullptr)) + ".wav";
    if (onState_) onState_(true);
    recordThread_ = std::thread(&AudioCapture::recordLoop, this);
}

void AudioCapture::stop() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void AudioCapture::recordLoop() {

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto t0 = std::chrono::steady_clock::now();
    pa_simple *pa = pa_simple_new(nullptr, "vinput-cap", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput Capture: PA error: %s\n", pa_strerror(error));
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

    fprintf(stderr, "Vinput Capture [timer] pa_open=%ldms record=%ldms pa_close=%ldms n_reads=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaOpen - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecordEnd - tPaOpen).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tPaClose - tRecordEnd).count(),
            nReads, samples_.size());

    std::vector<int16_t> batch;
    {
        std::lock_guard<std::mutex> lk(sampleMutex_);
        batch.swap(samples_);
    }

    if (!batch.empty()) {
        normalizeAndWriteWav(batch, wavPath_);
        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            samples_ = std::move(batch);
        }
    }
}

std::vector<int16_t> AudioCapture::takeSamples() {
    std::lock_guard<std::mutex> lk(sampleMutex_);
    return std::move(samples_);
}

void AudioCapture::normalizeAndWriteWav(std::vector<int16_t> &samples,
                                         const std::string &path) {
    auto t0 = std::chrono::steady_clock::now();

    ebur128_state *ebur = ebur128_init(1, 16000, EBUR128_MODE_I);
    ebur128_add_frames_short(ebur, samples.data(), samples.size());
    double loudness = 0.0;
    ebur128_loudness_global(ebur, &loudness);
    ebur128_destroy(&ebur);

    constexpr double kTargetLUFS = -16.0;
    double gain;
    if (loudness < -70.0 || !std::isfinite(loudness)) {
        fprintf(stderr, "Vinput Capture: audio too quiet (%.1f LUFS), skip norm\n", loudness);
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
        fprintf(stderr, "Vinput Capture: cannot write WAV to %s\n", path.c_str());
        return;
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

    fprintf(stderr, "Vinput Capture [timer] ebur128=%ldms gain=%ldms wav_write=%ldms loudness=%.1f gain=%.2f samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - tGain).count(),
            loudness, gain, samples.size());
}

} // namespace vinput
