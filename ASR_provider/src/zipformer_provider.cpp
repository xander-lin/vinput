#include "zipformer_provider.h"

#include <sherpa-onnx/c-api/cxx-api.h>
#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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
    if (recogThread_.joinable()) recogThread_.detach();
    delete static_cast<sherpa_onnx::cxx::OnlineRecognizer *>(recognizer_);
    unlink(tempWavPath_.c_str());
}

void ZipformerAsrProvider::setConfig(const std::string &key,
                                      const std::string &value) {
    if (key == "model_dir") modelDir_ = value;
}

void ZipformerAsrProvider::start() {
    if (recording_) return;

    if (!recognizer_) {
        auto dir = expandPath(modelDir_);

        sherpa_onnx::cxx::OnlineRecognizerConfig config;
        config.model_config.transducer.encoder = dir + "/encoder-epoch-99-avg-1.onnx";
        config.model_config.transducer.decoder = dir + "/decoder-epoch-99-avg-1.onnx";
        config.model_config.transducer.joiner = dir + "/joiner-epoch-99-avg-1.onnx";
        config.model_config.tokens = dir + "/tokens.txt";
        config.model_config.num_threads = 4;
        config.model_config.provider = "cpu";

        auto *rec = new sherpa_onnx::cxx::OnlineRecognizer(
            sherpa_onnx::cxx::OnlineRecognizer::Create(config));
        recognizer_ = rec;
        fprintf(stderr, "Vinput: Zipformer model loaded\n");
    }

    recording_ = true;
    if (onState_) onState_(true);
    recordThread_ = std::thread([this]() { recordThread(); });
}

void ZipformerAsrProvider::stop() {
    if (!recording_) return;
    recording_ = false;
    if (recordThread_.joinable()) recordThread_.join();
    if (onState_) onState_(false);

    if (recogThread_.joinable()) recogThread_.join();
    auto wav = tempWavPath_;
    auto rec = recognizer_;
    auto onR = onResult_;
    auto onE = onError_;
    recogThread_ = std::thread([wav, rec, onR, onE]() {
        recognizeThread(wav, rec, onR, onE);
    });
}

void ZipformerAsrProvider::recognizeThread(std::string wavPath,
                                            void *recognizer,
                                            AsrResultCallback onR,
                                            AsrErrorCallback onE) {
    auto *rec = static_cast<sherpa_onnx::cxx::OnlineRecognizer *>(recognizer);
    if (!rec) { if (onE) onE("Zipformer: no recognizer"); return; }

    FILE *f = fopen(wavPath.c_str(), "rb");
    if (!f) { if (onE) onE("Zipformer: no audio"); return; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    rewind(f);
    if (size <= 44) { fclose(f); if (onE) onE("Zipformer: empty"); return; }

    fseek(f, 44, SEEK_SET);
    int numSamples = (size - 44) / 2;
    std::vector<float> samples(numSamples);
    std::vector<int16_t> raw(numSamples);
    fread(raw.data(), 2, numSamples, f);
    fclose(f);
    for (int i = 0; i < numSamples; i++)
        samples[i] = raw[i] / 32768.0f;

    // Simulate streaming: feed all frames at once, decode, get result
    auto stream = rec->CreateStream();
    stream.AcceptWaveform(16000, samples.data(), numSamples);
    stream.InputFinished();

    std::string text;
    while (rec->IsReady(&stream)) rec->Decode(&stream);
    auto result = rec->GetResult(&stream);
    text = result.text;

    fprintf(stderr, "Vinput: Zipformer text = %s\n", text.c_str());
    if (onR && !text.empty()) onR(text, true);
    else if (onE) onE("Zipformer: empty result");
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
