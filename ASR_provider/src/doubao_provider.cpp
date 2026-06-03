#include "doubao_provider.h"

#include <pulse/simple.h>
#include <pulse/error.h>
#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

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

    // 常驻录音流 + 单线程同时做 keep-alive 和录音发送
    pa_sample_spec ss;
    ss.format = PA_SAMPLE_S16LE;
    ss.rate = 16000;
    ss.channels = 1;
    int error = 0;
    paStream_ = pa_simple_new(nullptr, "vinput-doubao-keep",
                              PA_STREAM_RECORD, nullptr, "voice",
                              &ss, nullptr, nullptr, &error);
    if (!paStream_) {
        fprintf(stderr, "Vinput Doubao: keep-alive PA error: %s\n",
                pa_strerror(error));
        return;
    }
    fprintf(stderr, "Vinput Doubao: keep-alive stream started\n");

    keepAliveThread_ = std::thread([this]() {
        uint8_t buf[6400];  // 200ms chunks
        int err = 0;
        while (keepAliveRunning_) {
            if (pa_simple_read(paStream_, buf, sizeof(buf), &err) < 0)
                break;
            if (sendRunning_) {
                std::lock_guard<std::mutex> lk(sendMutex_);
                if (sendFd_ >= 0)
                    write(sendFd_, buf, sizeof(buf));
                auto *p = reinterpret_cast<int16_t *>(buf);
                allSamples_.insert(allSamples_.end(), p, p + sizeof(buf) / 2);
            }
        }
    });
}

DoubaoAsrProvider::~DoubaoAsrProvider() {
    keepAliveRunning_ = false;
    if (keepAliveThread_.joinable()) keepAliveThread_.join();
    if (paStream_) {
        pa_simple_free(paStream_);
        paStream_ = nullptr;
    }
    if (sendFd_ >= 0) close(sendFd_);
    if (recvFd_ >= 0) close(recvFd_);
}

void DoubaoAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "script") scriptPath_ = value;
}

void DoubaoAsrProvider::start() {
    if (sendRunning_) return;
    allSamples_.clear();

    // 创建管道
    int stdinPipe[2], stdoutPipe[2];
    if (pipe(stdinPipe) < 0 || pipe(stdoutPipe) < 0) {
        if (onError_) onError_("Doubao: pipe failed");
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        return;
    }

    if (pid == 0) {
        dup2(stdinPipe[0], STDIN_FILENO);
        dup2(stdoutPipe[1], STDOUT_FILENO);
        close(stdinPipe[0]); close(stdinPipe[1]);
        close(stdoutPipe[0]); close(stdoutPipe[1]);
        execl("/usr/bin/python3", "python3", scriptPath_.c_str(), nullptr);
        _exit(127);
    }

    close(stdinPipe[0]);
    close(stdoutPipe[1]);
    sendFd_ = stdinPipe[1];
    recvFd_ = stdoutPipe[0];
    childPid_ = pid;

    sendRunning_ = true;
    if (onState_) onState_(true);

    // 结果读取线程
    std::thread([this]() {
        FILE *f = fdopen(recvFd_, "r");
        if (!f) return;

        char line[16384];
        while (sendRunning_ && fgets(line, sizeof(line), f)) {
            const char *ts = strstr(line, "\"text\"");
            const char *fs = strstr(line, "\"is_final\"");
            const char *es = strstr(line, "\"error\"");

            if (es) {
                fprintf(stderr, "Vinput Doubao error: %s\n", line);
                if (onError_) onError_("Doubao: " + std::string(line));
                continue;
            }
            if (!ts) continue;

            ts = strchr(ts + 7, '"');
            if (!ts) continue;
            ts++;
            const char *te = strchr(ts, '"');
            if (!te) continue;
            std::string text(ts, te - ts);

            bool isFinal = false;
            if (fs) {
                fs = strchr(fs + 10, ':');
                if (fs) {
                    fs++;
                    while (*fs == ' ') fs++;
                    isFinal = (*fs == 't' || *fs == 'T');
                }
            }

            fprintf(stderr, "Vinput Doubao: text=\"%s\" final=%d\n",
                    text.c_str(), isFinal);
            if (onResult_ && !text.empty())
                onResult_(text, isFinal);
            if (isFinal) break;
        }
        fclose(f);
        recvFd_ = -1;
        if (childPid_) { waitpid(childPid_, nullptr, 0); childPid_ = 0; }
        if (onState_) onState_(false);
    }).detach();
}

void DoubaoAsrProvider::stop() {
    sendRunning_ = false;
    // 关闭写入端，让 Python 脚本收到 EOF 后发最后一包
    {
        std::lock_guard<std::mutex> lk(sendMutex_);
        if (sendFd_ >= 0) { close(sendFd_); sendFd_ = -1; }
    }
    // 保存 WAV
    if (!allSamples_.empty()) {
        FILE *f = fopen(tempWavPath_.c_str(), "wb");
        if (f) {
            long ds = (long)allSamples_.size() * 2;
            fwrite("RIFF", 1, 4, f);
            uint32_t cs = 36 + (uint32_t)ds; fwrite(&cs, 4, 1, f);
            fwrite("WAVE", 1, 4, f); fwrite("fmt ", 1, 4, f);
            uint32_t s1=16, sr=16000, br=32000;
            uint16_t ff=1, ch=1, bps=16, ba=2;
            fwrite(&s1,4,1,f); fwrite(&ff,2,1,f); fwrite(&ch,2,1,f);
            fwrite(&sr,4,1,f); fwrite(&br,4,1,f); fwrite(&ba,2,1,f); fwrite(&bps,2,1,f);
            fwrite("data",1,4,f); fwrite(&ds,4,1,f);
            fwrite(allSamples_.data(), 2, allSamples_.size(), f);
            fclose(f);
            fprintf(stderr, "Vinput Doubao: saved %zu samples to %s\n",
                    allSamples_.size(), tempWavPath_.c_str());
        }
    }
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
