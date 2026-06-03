#include "doubao_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>

namespace vinput {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

DoubaoAsrProvider::DoubaoAsrProvider() {
    scriptPath_ = expandPath("~/.local/share/vinput/scripts/doubao_asr.py");
    tempWavPath_ = "/tmp/vinput_doubao_" + std::to_string(getpid()) + "_"
                   + std::to_string(time(nullptr)) + ".wav";
}

DoubaoAsrProvider::~DoubaoAsrProvider() {
    if (childPid_) {
        kill(childPid_, SIGTERM);
        waitpid(childPid_, nullptr, 0);
    }
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "script") scriptPath_ = value;
}

void DoubaoAsrProvider::start() {
    if (running_) return;
    running_ = true;
    if (onState_) onState_(true);

    // 创建管道: stdin(写) → child, stdout(读) ← child
    int stdinPipe[2], stdoutPipe[2];
    if (pipe(stdinPipe) < 0 || pipe(stdoutPipe) < 0) {
        if (onError_) onError_("Doubao: pipe failed");
        running_ = false;
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (onError_) onError_("Doubao: fork failed");
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        running_ = false;
        return;
    }

    if (pid == 0) {
        // --- child: 运行 Python 脚本 ---
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);

        execl("/usr/bin/python3", "python3", scriptPath_.c_str(), nullptr);
        _exit(127);
    }

    // --- parent ---
    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    childStdin_ = stdinPipe[1];
    childStdout_ = stdoutPipe[0];
    childPid_ = pid;

    // 结果读取线程
    std::thread resultReader([this]() { readResults(); });
    resultReader.detach();

    // 录音发送线程
    std::thread sender([this]() { recordAndSend(); });
    sender.detach();
}

void DoubaoAsrProvider::stop() {
    running_ = false;
}

void DoubaoAsrProvider::recordAndSend() {
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;

    int error = 0;
    auto *pa = pa_simple_new(nullptr, "vinput-doubao", PA_STREAM_RECORD,
                             nullptr, "voice", &ss, nullptr, nullptr, &error);
    if (!pa) {
        fprintf(stderr, "Vinput Doubao: PA error: %s\n", pa_strerror(error));
        if (onError_) onError_("Doubao: microphone error");
        close(childStdin_);
        return;
    }

    // 200ms 音频 = 16000 * 0.2 * 2 = 6400 bytes
    constexpr int kChunkBytes = 6400;
    std::vector<int16_t> allSamples;
    uint8_t buf[kChunkBytes];

    while (running_) {
        if (pa_simple_read(pa, buf, kChunkBytes, &error) < 0) break;
        write(childStdin_, buf, kChunkBytes);
        auto *p = reinterpret_cast<int16_t *>(buf);
        allSamples.insert(allSamples.end(), p, p + kChunkBytes / 2);
    }

    pa_simple_free(pa);
    close(childStdin_);  // EOF → Python 脚本发送最后一包

    writeWav(allSamples, tempWavPath_);
    fprintf(stderr, "Vinput Doubao: recorded %zu samples to %s\n",
            allSamples.size(), tempWavPath_.c_str());
}

void DoubaoAsrProvider::readResults() {
    FILE *f = fdopen(childStdout_, "r");
    if (!f) return;

    char line[16384];
    while (fgets(line, sizeof(line), f)) {
        // 解析 JSONL: {"text": "...", "is_final": true/false}
        auto *p = line;
        while (*p == ' ' || *p == '\t') p++;

        // 简单解析: 找 "text" 和 "is_final"
        const char *textStart = strstr(p, "\"text\"");
        const char *finalStart = strstr(p, "\"is_final\"");

        if (!textStart) {
            // 可能是错误: {"error": "..."}
            if (strstr(p, "\"error\"")) {
                fprintf(stderr, "Vinput Doubao error: %s\n", p);
                if (onError_) onError_("Doubao: " + std::string(p));
            }
            continue;
        }

        // 提取 text
        textStart = strchr(textStart + 7, '"');
        if (!textStart) continue;
        textStart++;
        const char *textEnd = strchr(textStart, '"');
        if (!textEnd) continue;
        std::string text(textStart, textEnd - textStart);

        bool isFinal = false;
        if (finalStart) {
            finalStart = strchr(finalStart + 10, ':');
            if (finalStart) {
                finalStart++;
                while (*finalStart == ' ' || *finalStart == '\t') finalStart++;
                isFinal = (*finalStart == 't' || *finalStart == 'T');
            }
        }

        fprintf(stderr, "Vinput Doubao: text=\"%s\" final=%d\n",
                text.c_str(), isFinal);

        if (!text.empty()) {
            if (isFinal) finalText_ = text;
            else partialText_ = text;

            if (onResult_) {
                if (isFinal) {
                    onResult_(text, true);
                } else {
                    onResult_(text, false);
                }
            }
        }

        if (isFinal) break;
    }

    fclose(f);
    childStdout_ = -1;

    if (childPid_) {
        waitpid(childPid_, nullptr, 0);
        childPid_ = 0;
    }
    if (onState_) onState_(false);
    running_ = false;
}

void DoubaoAsrProvider::writeWav(const std::vector<int16_t> &samples,
                                  const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
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
}

std::unique_ptr<IAsrProvider> DoubaoAsrProviderFactory::create() {
    return std::make_unique<DoubaoAsrProvider>();
}

static bool _doubaoReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<DoubaoAsrProviderFactory>());
    return true;
}();

} // namespace vinput
