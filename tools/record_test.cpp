// 独立录音测试程序
// 编译: g++ -std=c++17 record_test.cpp -o record_test -lpulse-simple -lpulse
// 运行: ./record_test [录音秒数] [输出wav路径]
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <chrono>

using Clock = std::chrono::steady_clock;

static void writeWav(const char *path, const std::vector<int16_t> &samples, int rate) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "Cannot open %s\n", path); return; }

    long dataSize = (long)samples.size() * 2;
    fwrite("RIFF", 1, 4, f);
    uint32_t chunkSize = 36 + (uint32_t)dataSize; fwrite(&chunkSize, 4, 1, f);
    fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f);
    uint32_t sub1 = 16, sr = (uint32_t)rate, br = (uint32_t)rate * 2;
    uint16_t fmt = 1, ch = 1, bps = 16, ba = 2;
    fwrite(&sub1, 4, 1, f); fwrite(&fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f); fwrite(&sr, 4, 1, f);
    fwrite(&br, 4, 1, f); fwrite(&ba, 2, 1, f);
    fwrite(&bps, 2, 1, f);
    fwrite("data", 1, 4, f);
    uint32_t ds = (uint32_t)dataSize; fwrite(&ds, 4, 1, f);
    fwrite(samples.data(), 2, samples.size(), f);
    fclose(f);
}

int main(int argc, char **argv) {
    int duration = 5;
    const char *outPath = "/tmp/record_test.wav";

    if (argc > 1) duration = atoi(argv[1]);
    if (argc > 2) outPath = argv[2];

    printf("Recording %d seconds to %s ...\n", duration, outPath);

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    printf("PA connecting...\n");
    auto t0 = Clock::now();
    auto *pa = pa_simple_new(nullptr, "record-test", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    auto t1 = Clock::now();
    if (!pa) {
        fprintf(stderr, "PulseAudio error: %s\n", pa_strerror(error));
        return 1;
    }
    printf("PA connected in %.0f ms\n",
           std::chrono::duration<double, std::milli>(t1 - t0).count());

    std::vector<int16_t> samples;
    uint8_t buf[4096];
    auto end = Clock::now() + std::chrono::seconds(duration);

    while (Clock::now() < end) {
        if (pa_simple_read(pa, buf, sizeof(buf), &error) < 0) {
            fprintf(stderr, "Read error: %s\n", pa_strerror(error));
            break;
        }
        auto *p = reinterpret_cast<int16_t *>(buf);
        samples.insert(samples.end(), p, p + sizeof(buf) / 2);
    }
    auto t2 = Clock::now();

    pa_simple_free(pa);

    if (samples.empty()) {
        fprintf(stderr, "No samples recorded\n");
        return 1;
    }

    int16_t maxAbs = 0;
    for (auto s : samples) {
        int16_t a = s >= 0 ? s : (int16_t)-s;
        if (a > maxAbs) maxAbs = a;
    }

    writeWav(outPath, samples, 16000);

    double secs = (double)samples.size() / 16000.0;
    double elapsed = std::chrono::duration<double>(t2 - t1).count();
    printf("Done: %zu samples (%.2f s), wall=%.2f, max_abs=%d, file: %s\n",
           samples.size(), secs, elapsed, (int)maxAbs, outPath);
    return 0;
}
