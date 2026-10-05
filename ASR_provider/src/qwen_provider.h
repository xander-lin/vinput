#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include "asr_provider.h"

namespace vinput {

// Everything the Qwen request needs, resolved from qwen.json (+ setConfig
// overrides) on the worker thread right before each request. The config file
// is re-read per request so model switches need no restart or recompile.
//
// API key lifecycle: a plaintext api_key in qwen.json means "set/update the
// key" — it is imported into the encrypted secret store and stripped from
// the file; afterwards the key is read from the store (see secret_store.h).
struct QwenSettings {
    std::string apiKey;             // resolved key (override > file > keyring)
    std::string model;
    std::string endpoint;
    std::string requestStyle;       // "auto" (default) | "input_audio" | "legacy"
    std::string languageHintsRaw;   // raw JSON array, e.g. ["zh","en"]; empty = auto
    std::string vocabularyRaw;      // raw JSON object {word: weight}; empty = none
    std::string vocabularyId;       // precompiled hotword list id; empty = none
    bool keepDialect = false;       // qwen-audio-3.1-asr-flash: keep dialect text
    bool speakerDiarization = false; // qwen-audio-3.1-asr-flash only
};

class QwenAsrProvider : public IAsrProvider {
public:
    QwenAsrProvider();
    ~QwenAsrProvider() override;

    void transcribe(std::vector<int16_t> samples, const std::string &wavPath) override;
    void setConfig(const std::string &key, const std::string &value) override;

private:
    struct Task {
        std::vector<int16_t> samples;
        std::string wavPath;
        long timeout;
        std::shared_ptr<std::atomic_bool> cancel;
        AsrResultCallback onResult;
        AsrErrorCallback onError;
        uint64_t diagnosticId;
    };
    struct WorkerState {
        std::mutex mutex;
        std::condition_variable ready;
        std::deque<Task> tasks;
        std::shared_ptr<std::atomic_bool> activeCancel;
        bool stopping = false;
    };

    void workerLoop(const std::shared_ptr<WorkerState> &state);
    static void processRecording(std::vector<int16_t> samples,
                                 const std::string &wavPath,
                                 const QwenSettings &settings, long timeout,
                                 std::shared_ptr<std::atomic_bool> cancel,
                                 AsrResultCallback onR, AsrErrorCallback onE,
                                 uint64_t diagnosticId);

    QwenSettings resolveSettings();

    std::string apiKeyOverride_;   // set via setConfig; wins over everything
    std::string keyringCache_;     // cached keyring lookup (worker thread only)
    long timeout_ = 60;
    std::shared_ptr<WorkerState> state_;
    std::thread worker_;
};

class QwenAsrProviderFactory : public IAsrProviderFactory {
public:
    std::string id() const override { return "qwen"; }
    std::string name() const override { return "Qwen ASR (Alibaba DashScope)"; }
    std::unique_ptr<IAsrProvider> create() override;
};

} // namespace vinput
