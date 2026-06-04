#include "fire_red_provider.h"
#include "vinput_config.h"

#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <spawn.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

namespace vinput {

static std::string expandPath(const std::string &p) {
    if (!p.empty() && p[0] == '~') {
        const char *h = getenv("HOME");
        if (h) return std::string(h) + p.substr(1);
    }
    return p;
}

FireRedAsrProvider::FireRedAsrProvider()
    : modelDir_("~/.local/share/vinput/models/sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26") {
    auto adv = advancedSection("fire_red");
    if (!adv.empty()) {
        auto d = jsonStr(adv, "model_dir");
        if (!d.empty()) modelDir_ = d;
        numThreads_ = jsonInt(adv, "num_threads", numThreads_);
        auto b = jsonStr(adv, "bin_path");
        if (!b.empty()) sherpaBin_ = b;
    }
}

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
        if (pipe2(pipefd, O_CLOEXEC) < 0) {
            if (onE) onE("FireRed: pipe failed");
            return;
        }

        auto sherpaBin = expandPath(sherpaBin_);
        std::vector<std::string> args = {
            sherpaBin,
            "--fire-red-asr-encoder=" + dir + "/encoder.int8.onnx",
            "--fire-red-asr-decoder=" + dir + "/decoder.int8.onnx",
            "--tokens=" + dir + "/tokens.txt",
            "--num-threads=" + std::to_string(numThreads_),
            wav
        };
        std::vector<const char*> argv;
        for (auto &a : args) argv.push_back(a.c_str());
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addclose(&actions, pipefd[0]);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipefd[1]);

        pid_t pid;
        int ret = posix_spawn(&pid, sherpaBin.c_str(), &actions, nullptr,
                              (char *const *)argv.data(), ::environ);
        posix_spawn_file_actions_destroy(&actions);
        close(pipefd[1]);

        if (ret != 0) {
            close(pipefd[0]);
            if (onE) onE("FireRed: spawn failed");
            return;
        }

        std::string output;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            output.append(buf, (size_t)n);
        }
        close(pipefd[0]);

        int status;
        if (waitpid(pid, &status, 0) == -1) {
            fprintf(stderr, "Vinput FireRed: waitpid failed\n");
            unlink(wav.c_str());
            if (onE) onE("FireRed: recognition failed");
            return;
        }
        auto tRecv = std::chrono::steady_clock::now();

        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            fprintf(stderr, "Vinput FireRed: child exit=%d\n",
                    WIFEXITED(status) ? WEXITSTATUS(status) : -1);
            unlink(wav.c_str());
            if (onE) onE("FireRed: recognition failed");
            return;
        }

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
