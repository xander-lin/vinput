#include "fire_red_provider.h"

#include <unistd.h>
#include <sys/wait.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>

namespace vinput {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

FireRedAsrProvider::FireRedAsrProvider()
    : modelDir_("~/.local/share/vinput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26") {}

void FireRedAsrProvider::setConfig(const std::string &key,
                                    const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void FireRedAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    runTranscribe(wavPath, expandPath(modelDir_), onResult_, onError_);
}

void FireRedAsrProvider::runTranscribe(const std::string &wav,
                                          const std::string &dir,
                                          AsrResultCallback onR,
                                          AsrErrorCallback onE) {
    std::thread([=]() {
        auto t0 = std::chrono::steady_clock::now();

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            if (onE) onE("FireRed: pipe failed");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]); close(pipefd[1]);
            if (onE) onE("FireRed: fork failed");
            return;
        }
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            auto sherpaBin = expandPath("~/.local/share/vinput/sherpa-onnx/bin/sherpa-onnx-offline");
            std::string encoder = "--fire-red-asr-encoder=" + dir + "/encoder.int8.onnx";
            std::string decoder = "--fire-red-asr-decoder=" + dir + "/decoder.int8.onnx";
            std::string tokens  = "--tokens="  + dir + "/tokens.txt";

            execl(sherpaBin.c_str(), sherpaBin.c_str(),
                  encoder.c_str(),
                  decoder.c_str(),
                  tokens.c_str(),
                  "--num-threads=30",
                  wav.c_str(),
                  nullptr);
            _exit(127);
        }

        close(pipefd[1]);

        std::string output;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output.append(buf, (size_t)n);
        }
        close(pipefd[0]);

        int status;
        waitpid(pid, &status, 0);
        auto tRecv = std::chrono::steady_clock::now();

        auto pos = output.rfind("\"text\"");
        std::string text;
        if (pos != std::string::npos) {
            pos += 9;
            auto end = output.find('"', pos);
            if (end != std::string::npos)
                text = output.substr(pos, end - pos);
        }

        auto tParse = std::chrono::steady_clock::now();
        fprintf(stderr, "Vinput FireRed [timer] exec_total=%ldms parse=%ldms text=\"%s\"\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.c_str());

        unlink(wav.c_str());
        if (onR && !text.empty()) onR(text, true);
        else if (onE) onE("FireRed: empty result");
    }).detach();
}

std::unique_ptr<IAsrProvider> FireRedAsrProviderFactory::create() {
    return std::make_unique<FireRedAsrProvider>();
}

static bool _frReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<FireRedAsrProviderFactory>());
    return true;
}();

} // namespace vinput
