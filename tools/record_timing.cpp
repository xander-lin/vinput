// 录音逐次耗时测试
#include <pulse/simple.h>
#include <pulse/error.h>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <cstdint>
#include <unistd.h>
#include <chrono>
#include <cmath>

using Clock = std::chrono::steady_clock;

int main(int argc, char **argv) {
    int duration = 5, bufSize = 4096;
    if (argc > 1) duration = atoi(argv[1]);
    if (argc > 2) bufSize = atoi(argv[2]);

    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto t0 = Clock::now();
    auto *pa = pa_simple_new(nullptr, "record-test", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    auto t1 = Clock::now();
    if (!pa) { fprintf(stderr, "PA error\n"); return 1; }

    printf("PA connect: %.0f ms, buf=%d bytes (%.1f ms)\n",
           std::chrono::duration<double,std::milli>(t1-t0).count(),
           bufSize, bufSize / 32.0);

    std::vector<double> readMs;
    uint8_t *buf = new uint8_t[bufSize];
    auto end = Clock::now() + std::chrono::seconds(duration);
    int iter = 0, totalSamples = 0;

    while (Clock::now() < end) {
        auto r0 = Clock::now();
        if (pa_simple_read(pa, buf, (size_t)bufSize, &error) < 0) {
            fprintf(stderr, "Read err at iter %d: %s\n", iter, pa_strerror(error));
            break;
        }
        auto r1 = Clock::now();
        double ms = std::chrono::duration<double,std::milli>(r1-r0).count();
        readMs.push_back(ms);
        totalSamples += bufSize / 2;
        iter++;
    }
    auto t2 = Clock::now();
    pa_simple_free(pa);
    delete[] buf;

    double wallSec = std::chrono::duration<double>(t2 - t1).count();
    double audioSec = (double)totalSamples / 16000.0;

    // 统计
    double sum = 0, minMs = 1e9, maxMs = 0;
    for (double d : readMs) { sum+=d; if(d<minMs)minMs=d; if(d>maxMs)maxMs=d; }
    double avg = readMs.empty() ? 0 : sum / readMs.size();
    double var = 0;
    for (double d : readMs) { double x=d-avg; var+=x*x; }
    double stddev = readMs.empty() ? 0 : sqrt(var / readMs.size());

    printf("buf=%d  iter=%d  wall=%.2fs  audio=%.2fs  eff=%.0f%%\n",
           bufSize, iter, wallSec, audioSec, 100*audioSec/wallSec);
    printf("Read times: avg=%.1f  min=%.1f  max=%.1f  std=%.1f ms\n",
           avg, minMs, maxMs, stddev);
    printf("Expected read: %.1f ms\n", bufSize / 32.0);
    return 0;
}
