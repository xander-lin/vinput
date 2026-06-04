#include "audio_capture.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>

static std::vector<int16_t> readWav(const char *path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "Cannot open %s\n", path);
        return {};
    }

    char riff[5] = {};
    f.read(riff, 4);
    if (strncmp(riff, "RIFF", 4) != 0) {
        fprintf(stderr, "Not a RIFF file\n");
        return {};
    }

    f.seekg(22, std::ios::beg);
    uint16_t channels = 0, bps = 0;
    f.read(reinterpret_cast<char *>(&channels), 2);
    f.read(reinterpret_cast<char *>(&bps), 2);

    f.seekg(40, std::ios::beg);
    uint32_t dataSize = 0;
    f.read(reinterpret_cast<char *>(&dataSize), 4);

    f.seekg(44, std::ios::beg);
    size_t sampleCount = dataSize / 2;
    std::vector<int16_t> samples(sampleCount);
    f.read(reinterpret_cast<char *>(samples.data()), dataSize);

    fprintf(stderr, "Read %zu samples (ch=%d bps=%d)\n", samples.size(), (int)channels, (int)bps);
    return samples;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <input.wav> <output.wav> [denoise=0|1]\n", argv[0]);
        return 1;
    }

    const char *inPath = argv[1];
    const char *outPath = argv[2];
    bool denoise = (argc > 3) ? (atoi(argv[3]) != 0) : false;

    auto samples = readWav(inPath);
    if (samples.empty()) return 1;

    fprintf(stderr, "Processing %s -> %s (denoise=%d)\n", inPath, outPath, (int)denoise);
    vinput::AudioCapture::processSamples(samples, denoise);
    vinput::AudioCapture::writeWav(samples, outPath);

    fprintf(stderr, "Done: %s (%zu samples)\n", outPath, samples.size());
    return 0;
}
