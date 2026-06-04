#include <pulse/simple.h>
#include <pulse/error.h>
#include <signal.h>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <string>

#include "audio_capture.h"
#include "buffer_detect.h"

static volatile bool g_stop = false;
static void sigHandler(int) { g_stop = true; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <output.wav> [duration_sec=0] [denoise=0|1]\n", argv[0]);
        fprintf(stderr, "  duration_sec=0: record until Ctrl+C (SIGINT)\n");
        fprintf(stderr, "  denoise=1: enable speexdsp denoise\n");
        return 1;
    }

    const char *outPath = argv[1];
    int duration = (argc > 2) ? atoi(argv[2]) : 0;
    bool denoise = (argc > 3) ? (atoi(argv[3]) != 0) : false;

    signal(SIGINT, sigHandler);

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    fprintf(stderr, "Opening PulseAudio...\n");
    pa_simple *pa = pa_simple_new(nullptr, "vinput-test", PA_STREAM_RECORD,
                                  nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "PA error: %s\n", pa_strerror(error));
        return 1;
    }

    size_t bufferBytes = vinput::loadOrDetectBufferBytes();
    fprintf(stderr, "Buffer: %zu bytes\n", bufferBytes);

    std::vector<uint8_t> buf(bufferBytes);
    size_t frameCount = buf.size() / 2;
    std::vector<int16_t> samples;

    if (duration > 0) {
        fprintf(stderr, "Recording %ds to %s (denoise=%d)...\n", duration, outPath, (int)denoise);
    } else {
        fprintf(stderr, "Recording to %s (denoise=%d), press Ctrl+C to stop...\n", outPath, (int)denoise);
    }

    auto endTime = std::chrono::steady_clock::now() + std::chrono::seconds(duration);

    while (!g_stop) {
        if (pa_simple_read(pa, buf.data(), buf.size(), &error) < 0) {
            fprintf(stderr, "Read error: %s\n", pa_strerror(error));
            break;
        }
        auto *p = reinterpret_cast<int16_t *>(buf.data());
        samples.insert(samples.end(), p, p + frameCount);
        if (duration > 0 && std::chrono::steady_clock::now() >= endTime) break;
    }

    pa_simple_free(pa);
    fprintf(stderr, "\nRecorded %zu samples (%.1fs)\n", samples.size(), samples.size() / 16000.0);

    if (samples.empty()) return 1;

    auto t0 = std::chrono::steady_clock::now();
    vinput::AudioCapture::processSamples(samples, denoise);
    vinput::AudioCapture::writeWav(samples, outPath);

    auto t1 = std::chrono::steady_clock::now();
    fprintf(stderr, "Written: %s (%zu samples, %.1fs, pipeline=%ldms)\n",
            outPath, samples.size(), samples.size() / 16000.0,
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
    return 0;
}
