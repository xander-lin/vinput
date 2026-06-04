#include "zipformer_provider.h"

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

ZipformerAsrProvider::ZipformerAsrProvider()
    : modelDir_("~/.local/share/vinput/models/zipformer-zh-en") {}

void ZipformerAsrProvider::setConfig(const std::string &key,
                                      const std::string &value) {
    if (key == "model_dir") {
        modelDir_ = value;
    }
}

void ZipformerAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    runTranscribe(wavPath, expandPath(modelDir_), onResult_, onError_);
}

void ZipformerAsrProvider::runTranscribe(const std::string &wav,
                                          const std::string &dir,
                                          AsrResultCallback onR,
                                          AsrErrorCallback onE) {
    std::thread([=]() {
        auto t0 = std::chrono::steady_clock::now();

        int pipefd[2];
        if (pipe(pipefd) < 0) {
            if (onE) onE("Zipformer: pipe failed");
            return;
        }

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]); close(pipefd[1]);
            if (onE) onE("Zipformer: fork failed");
            return;
        }
        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDERR_FILENO);
            dup2(pipefd[1], STDOUT_FILENO);
            close(pipefd[1]);

            auto sherpaBin = expandPath("~/.local/share/vinput/sherpa-onnx/bin/sherpa-onnx");
            std::string encoder = "--encoder=" + dir + "/encoder-epoch-99-avg-1.onnx";
            std::string decoder = "--decoder=" + dir + "/decoder-epoch-99-avg-1.onnx";
            std::string joiner  = "--joiner="  + dir + "/joiner-epoch-99-avg-1.onnx";
            std::string tokens  = "--tokens="  + dir + "/tokens.txt";

            execl(sherpaBin.c_str(), sherpaBin.c_str(),
                  encoder.c_str(),
                  decoder.c_str(),
                  joiner.c_str(),
                  tokens.c_str(),
                  "--provider=cpu",
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
        fprintf(stderr, "Vinput Zipformer [timer] exec_total=%ldms parse=%ldms text=\"%s\"\n",
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tRecv - t0).count(),
                (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tRecv).count(),
                text.c_str());

        unlink(wav.c_str());
        if (onR && !text.empty()) onR(text, true);
        else if (onE) onE("Zipformer: empty result");
    }).detach();
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
