#include "audio_capture.h"
#include "buffer_detect.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <speex/speex_preprocess.h>
#include <unistd.h>
#include <cstdio>
#include <cmath>
#include <cstring>
#include <ctime>
#include <chrono>
#include <cstring>
#include <fstream>
#include <ebur128.h>
#include <cstdlib>

namespace vinput {

static std::string jsonGetString(const std::string &json, const std::string &key) {
    std::string q = "\"" + key + "\"";
    auto pos = json.find(q);
    if (pos == std::string::npos) return "";
    pos += q.size();
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':' || json[pos] == '\t'))
        pos++;
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        pos++;
        auto end = json.find('"', pos);
        if (end == std::string::npos) return "";
        return json.substr(pos, end - pos);
    }

    if (json[pos] == 't' || json[pos] == 'f') {
        return json.substr(pos, json.find_first_of(",}\n\r \t", pos) - pos);
    }

    return "";
}

static void loadAudioConfig(std::string &denoiseMethod) {
    const char *home = getenv("HOME");
    if (!home) return;
    std::string path = std::string(home) + "/.config/vinput/audio.json";
    std::ifstream f(path);
    if (!f) return;
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    auto val = jsonGetString(json, "denoise");
    if (val == "deepfilter") denoiseMethod = "deepfilter";
    else if (val == "speexdsp") denoiseMethod = "speexdsp";
    else if (val == "true") denoiseMethod = "speexdsp";
}

AudioCapture::AudioCapture() {
    loadAudioConfig(denoiseMethod_);
    if (!denoiseMethod_.empty())
        fprintf(stderr, "Vinput Capture: denoise=%s\n", denoiseMethod_.c_str());
}

AudioCapture::~AudioCapture() {
    stopRequested_ = true;
    if (recordThread_.joinable()) recordThread_.join();
}

void AudioCapture::processSamples(std::vector<int16_t> &samples, const std::string &denoiser) {
    auto t0 = std::chrono::steady_clock::now();

    double loudness = normalizeSamples(samples);
    bool isBlank = !hasVoice(samples);

    if (!isBlank && !denoiser.empty()) applyDenoise(samples, denoiser);

    trimSilence(samples);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Vinput Pipeline [summary] loudness=%.1f isBlank=%d denoiser=%s time=%ldms samples=%zu\n",
            loudness, (int)isBlank, denoiser.c_str(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
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
        // Step 1: ebur128 loudness normalization
        double loudness = normalizeSamples(batch);

        // Step 2: VAD check (but don't trim yet — denoiser needs leading noise)
        bool isBlank = !hasVoice(batch);
        bool wantDenoise = !denoiseMethod_.empty();

        // Step 3: denoise (with full audio for noise profile learning)
        if (!isBlank && wantDenoise) applyDenoise(batch, denoiseMethod_);

        // Step 4: trim silence (now that denoiser has processed the noise)
        trimSilence(batch);

        // Step 5: write WAV
        writeWav(batch, wavPath_);

        fprintf(stderr, "Vinput Capture [pipeline] loudness=%.1f isBlank=%d denoiser=%s samples=%zu\n",
                loudness, (int)isBlank, denoiseMethod_.c_str(), batch.size());

        {
            std::lock_guard<std::mutex> lk(sampleMutex_);
            samples_ = std::move(batch);
        }
    }
}

void AudioCapture::applyDenoise(std::vector<int16_t> &samples, const std::string &method) {
    if (method == "deepfilter") {
        dfDenoise(samples);
        return;
    }
    auto t0 = std::chrono::steady_clock::now();

    constexpr int kFrameSize = 320;
    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_DENOISE, &enable);
    int level = -15;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &level);

    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount; i++) {
        speex_preprocess_run(st, samples.data() + i * kFrameSize);
    }
    size_t remainder = samples.size() % kFrameSize;
    if (remainder > 0) {
        std::vector<int16_t> pad(samples.end() - remainder, samples.end());
        pad.resize(kFrameSize, 0);
        speex_preprocess_run(st, pad.data());
        std::copy(pad.begin(), pad.begin() + remainder, samples.end() - remainder);
    }

    speex_preprocess_state_destroy(st);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Vinput Capture [timer] denoise=%ldms samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            samples.size());
}

void AudioCapture::dfDenoise(std::vector<int16_t> &samples) {
    auto t0 = std::chrono::steady_clock::now();

    const char *home = getenv("HOME");
    std::string script = home ? std::string(home) + "/.local/share/vinput/scripts/df_denoise.py"
                              : "";
    std::string python = home ? std::string(home) + "/Work/Vinput/.venv/bin/python3" : "";

    std::string tmpIn = "/tmp/vinput_df_" + std::to_string(getpid()) + "_in.wav";
    std::string tmpOut = "/tmp/vinput_df_" + std::to_string(getpid()) + "_out.wav";

    writeWav(samples, tmpIn);

    std::string cmd = python + " " + script + " " + tmpIn + " " + tmpOut + " 2>/dev/null";
    int ret = std::system(cmd.c_str());

    if (ret == 0) {
        std::ifstream f(tmpOut, std::ios::binary);
        if (f) {
            f.seekg(0, std::ios::end);
            size_t size = f.tellg();
            f.seekg(44, std::ios::beg);
            size_t dataSize = size - 44;
            samples.resize(dataSize / 2);
            f.read(reinterpret_cast<char *>(samples.data()), dataSize);
        }
    }

    unlink(tmpIn.c_str());
    unlink(tmpOut.c_str());

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Vinput Capture [timer] df_denoise=%ldms ret=%d samples=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count(),
            ret, samples.size());
}

std::vector<int16_t> AudioCapture::takeSamples() {
    std::lock_guard<std::mutex> lk(sampleMutex_);
    return std::move(samples_);
}

double AudioCapture::normalizeSamples(std::vector<int16_t> &samples) {
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
    fprintf(stderr, "Vinput Capture [timer] ebur128=%ldms gain=%ldms loudness=%.1f gain=%.2f\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEbur - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tGain - tEbur).count(),
            loudness, gain);

    return loudness;
}

bool AudioCapture::hasVoice(const std::vector<int16_t> &samples) {
    constexpr int kFrameSize = 320;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 80;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 50;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    bool hasVoice = false;
    size_t frameCount = samples.size() / kFrameSize;
    for (size_t i = 0; i < frameCount && !hasVoice; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            hasVoice = true;
    }
    size_t remainder = samples.size() % kFrameSize;
    if (!hasVoice && remainder > 0) {
        int16_t frame[kFrameSize] = {};
        memcpy(frame, samples.data() + frameCount * kFrameSize, remainder * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            hasVoice = true;
    }

    speex_preprocess_state_destroy(st);
    return hasVoice;
}

void AudioCapture::trimSilence(std::vector<int16_t> &samples) {
    constexpr int kFrameSize = 320;
    constexpr size_t kPadFrames = 1;
    constexpr size_t kMinHeadSamples = 3200;

    SpeexPreprocessState *st = speex_preprocess_state_init(kFrameSize, 16000);
    int enable = 1;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_VAD, &enable);
    int probStart = 80;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_START, &probStart);
    int probContinue = 50;
    speex_preprocess_ctl(st, SPEEX_PREPROCESS_SET_PROB_CONTINUE, &probContinue);

    size_t frameCount = samples.size() / kFrameSize;
    std::vector<bool> voiceFrames(frameCount, false);

    for (size_t i = 0; i < frameCount; i++) {
        int16_t frame[kFrameSize];
        memcpy(frame, samples.data() + i * kFrameSize, kFrameSize * sizeof(int16_t));
        if (speex_preprocess_run(st, frame))
            voiceFrames[i] = true;
    }

    speex_preprocess_state_destroy(st);

    size_t firstVoice = 0;
    while (firstVoice < frameCount && !voiceFrames[firstVoice]) firstVoice++;
    size_t lastVoice = frameCount;
    while (lastVoice > 0 && !voiceFrames[lastVoice - 1]) lastVoice--;

    if (firstVoice >= lastVoice) return;

    firstVoice = firstVoice > kPadFrames ? firstVoice - kPadFrames : 0;
    lastVoice = std::min(lastVoice + kPadFrames, frameCount);

    size_t trimStart = firstVoice * kFrameSize;
    size_t trimEnd = lastVoice * kFrameSize;

    if (trimStart < kMinHeadSamples) trimStart = kMinHeadSamples;

    size_t origSize = samples.size();
    if (trimStart >= origSize) return;

    if (trimStart > 0) {
        samples.erase(samples.begin(), samples.begin() + trimStart);
    }
    size_t shiftedEnd = trimEnd - trimStart;
    if (shiftedEnd < samples.size()) {
        samples.erase(samples.begin() + shiftedEnd, samples.end());
    }

    fprintf(stderr, "Vinput Capture: trimmed %zu leading + %zu trailing samples (orig=%zu now=%zu)\n",
            trimStart, origSize - trimEnd, origSize, samples.size());
}

void AudioCapture::writeWav(const std::vector<int16_t> &samples, const std::string &path) {
    auto t0 = std::chrono::steady_clock::now();

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
    fprintf(stderr, "Vinput Capture [timer] wav_write=%ldms\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tWav - t0).count());
}

} // namespace vinput
