#include "qwen_provider.h"
#include "qwen_json.h"
#include "vinput_config.h"
#include "diagnostic_log.h"

#include <curl/curl.h>
#include <unistd.h>
#include <cstdio>
#include <chrono>
#include <thread>
#include <fstream>
#include <memory>

namespace vinput {

namespace {

// Defaults track the current DashScope generation (verified 2026-10). Both are
// overridable from qwen.json so future model/domain changes need no rebuild.
constexpr const char *kDefaultQwenModel = "qwen-audio-3.1-asr-flash";
constexpr const char *kDefaultQwenEndpoint =
    "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";

std::string base64Encode(const uint8_t *data, size_t len) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)data[i] << 16;
        if (i + 1 < len) v |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out += T[(v >> 18) & 0x3F];
        out += T[(v >> 12) & 0x3F];
        out += (i + 1 < len) ? T[(v >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? T[v & 0x3F] : '=';
    }
    return out;
}

size_t writeCb(void *ptr, size_t size, size_t nmemb, std::string *out) {
    out->append((const char *)ptr, size * nmemb);
    return size * nmemb;
}

std::string buildRequestBody(const QwenSettings &s, const std::string &dataUri) {
    std::string model = jsonEscape(s.model);
    if (qwenUsesLegacyRequest(s.model, s.requestStyle)) {
        return "{"
               "\"model\":\"" + model + "\","
               "\"input\":{"
               "\"messages\":["
               "{\"content\":[{\"audio\":\"" + dataUri + "\"}],\"role\":\"user\"}"
               "]"
               "},"
               "\"parameters\":{"
               "\"asr_options\":{"
               "\"enable_itn\":false"
               "}"
               "}"
               "}";
    }
    // Current generation: input_audio content object; parameters.format is
    // required and describes our own capture pipeline (16 kHz mono WAV).
    std::string body =
        "{"
        "\"model\":\"" + model + "\","
        "\"input\":{"
        "\"messages\":[{"
        "\"role\":\"user\","
        "\"content\":[{"
        "\"type\":\"input_audio\","
        "\"input_audio\":{\"data\":\"" + dataUri + "\"}"
        "}]"
        "}]"
        "},"
        "\"parameters\":{"
        "\"format\":\"wav\","
        "\"sample_rate\":\"16000\"";
    if (!s.languageHintsRaw.empty())
        body += ",\"language_hints\":" + s.languageHintsRaw;
    if (s.keepDialect) body += ",\"keep_dialect\":true";
    if (s.speakerDiarization) body += ",\"speaker_diarization_enabled\":true";
    if (!s.vocabularyRaw.empty()) body += ",\"vocabulary\":" + s.vocabularyRaw;
    if (!s.vocabularyId.empty())
        body += ",\"vocabulary_id\":\"" + jsonEscape(s.vocabularyId) + "\"";
    body += "}}";
    return body;
}

} // namespace

QwenAsrProvider::QwenAsrProvider() {
    auto adv = advancedSection("qwen");
    if (!adv.empty()) {
        timeout_ = (long)jsonInt(adv, "timeout_sec", (int)timeout_);
    }
    state_ = std::make_shared<WorkerState>();
    worker_ = std::thread([state = state_] { workerLoop(state); });
}

QwenAsrProvider::~QwenAsrProvider() {
    diagnosticLog().event("provider", "provider_shutdown", {
        {"provider", "qwen"},
        {"recognition_id", std::to_string(diagnosticId_)}
    });
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->stopping = true;
        if (state_->activeCancel) state_->activeCancel->store(true);
        for (const auto &task : state_->tasks) unlink(task.wavPath.c_str());
        state_->tasks.clear();
    }
    state_->ready.notify_one();
    joinAsrWorker(worker_);
}

void QwenAsrProvider::setConfig(const std::string &key, const std::string &value) {
    if (key == "api_key") apiKeyOverride_ = value;
}

QwenSettings QwenAsrProvider::resolveSettings() const {
    QwenSettings s;
    s.model = kDefaultQwenModel;
    s.endpoint = kDefaultQwenEndpoint;
    s.requestStyle = "auto";
    std::string json = readConfigFile("qwen.json");
    if (!json.empty()) {
        s.apiKey = qjsonStringValue(json, "api_key");
        std::string v = qjsonStringValue(json, "model");
        if (!v.empty()) s.model = v;
        v = qjsonStringValue(json, "endpoint");
        if (!v.empty()) s.endpoint = v;
        v = qjsonStringValue(json, "request_style");
        if (!v.empty()) s.requestStyle = v;
        s.languageHintsRaw = qjsonRawValue(json, "language_hints");
        s.vocabularyRaw = qjsonRawValue(json, "vocabulary");
        s.vocabularyId = qjsonStringValue(json, "vocabulary_id");
        s.keepDialect = qjsonRawValue(json, "keep_dialect") == "true";
        s.speakerDiarization =
            qjsonRawValue(json, "speaker_diarization") == "true";
    }
    if (!apiKeyOverride_.empty()) s.apiKey = apiKeyOverride_;
    return s;
}

void QwenAsrProvider::transcribe(std::vector<int16_t> samples, const std::string &wavPath) {
    Task task{std::move(samples), wavPath, resolveSettings(), timeout_,
              std::make_shared<std::atomic_bool>(false), onResult_, onError_,
              diagnosticId_};
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->stopping) {
            unlink(wavPath.c_str());
            return;
        }
        state_->tasks.push_back(std::move(task));
    }
    state_->ready.notify_one();
}

void QwenAsrProvider::workerLoop(const std::shared_ptr<WorkerState> &state) {
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(state->mutex);
            state->ready.wait(lock, [&] {
                return state->stopping || !state->tasks.empty();
            });
            if (state->stopping) return;
            task = std::move(state->tasks.front());
            state->tasks.pop_front();
            state->activeCancel = task.cancel;
        }
        diagnosticLog().event("provider", "request_worker_begin", {
            {"provider", "qwen"},
            {"recognition_id", std::to_string(task.diagnosticId)},
            {"wav_hash", hashDiagnosticValue(task.wavPath).substr(0, 16)}
        });
        processRecording(std::move(task.samples), task.wavPath,
                         task.settings, task.timeout,
                         std::move(task.cancel), std::move(task.onResult),
                         std::move(task.onError), task.diagnosticId);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->activeCancel.reset();
            if (state->stopping) return;
        }
    }
}

void QwenAsrProvider::processRecording(std::vector<int16_t> samples,
                                        const std::string &wavPath,
                                        const QwenSettings &settings, long timeout,
                                        std::shared_ptr<std::atomic_bool> cancel,
                                        AsrResultCallback onR,
                                        AsrErrorCallback onE,
                                        uint64_t diagnosticId) {
    fprintf(stderr, "Vinput Qwen: recorded %zu samples to %s (model=%s style=%s)\n",
            samples.size(), wavPath.c_str(), settings.model.c_str(),
            qwenUsesLegacyRequest(settings.model, settings.requestStyle)
                ? "legacy" : "input_audio");
    diagnosticLog().event("provider", "request_started", {
        {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
        {"model", settings.model},
        {"wav_hash", hashDiagnosticValue(wavPath).substr(0, 16)},
        {"sample_count", std::to_string(samples.size())}
    });
    struct Cleanup { std::string p; ~Cleanup() { unlink(p.c_str()); } } _wav{wavPath};

    if (settings.apiKey.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "missing_api_key"}
        });
        if (onE) onE("Qwen: missing api_key in ~/.config/vinput/qwen.json");
        return;
    }

    auto t0 = std::chrono::steady_clock::now();
    std::ifstream wf(wavPath, std::ios::binary);
    if (!wf) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "wav_read_failed"}
        });
        if (onE) onE("Qwen: failed to read WAV");
        return;
    }
    std::vector<uint8_t> wavData((std::istreambuf_iterator<char>(wf)),
                                  std::istreambuf_iterator<char>());
    if (wavData.empty()) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "empty_wav"}
        });
        if (onE) onE("Qwen: empty WAV file");
        return;
    }
    std::string b64 = base64Encode(wavData.data(), wavData.size());
    std::string dataUri = "data:audio/wav;base64," + b64;
    auto tEncode = std::chrono::steady_clock::now();

    CURL *curl = getCurl();
    if (!curl) {
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "curl_init_failed"}
        });
        if (onE) onE("Qwen: curl init failed");
        return;
    }
    std::string requestBody = buildRequestBody(settings, dataUri);

    curl_easy_reset(curl);
    std::string respBody;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers,
        ("Authorization: Bearer " + settings.apiKey).c_str());

    curl_easy_setopt(curl, CURLOPT_URL, settings.endpoint.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)requestBody.size());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &respBody);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, std::min(timeout, 10L));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeout);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    CurlCancellationScope cancellation(curl, cancel);

    CURLcode res = curl_easy_perform(curl);
    long httpCode = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    curl_slist_free_all(headers);

    auto tNetwork = std::chrono::steady_clock::now();

    fprintf(stderr, "Vinput Qwen: HTTP %ld\n", httpCode);
    fprintf(stderr, "Vinput Qwen: response bytes=%zu\n", respBody.size());

    if (res != CURLE_OK) {
        if (cancel->load()) {
            diagnosticLog().event("provider", "request_cancelled", {
                {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)}
            });
            return;
        }
        fprintf(stderr, "Vinput Qwen: transport failed, curl=%d (%s)\n",
                (int)res, curl_easy_strerror(res));
        evictCurlHandle();
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "transport"}, {"handle_evicted", "true"}
        });
        if (onE) {
            onE("Qwen: network request failed (" +
                std::string(curl_easy_strerror(res)) + ")");
        }
        return;
    }
    if (httpCode != 200) {
        std::string detail = qwenExtractErrorDetail(respBody);
        fprintf(stderr, "Vinput Qwen: HTTP %ld error: %s\n", httpCode,
                detail.c_str());
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "http_status"}, {"http_code", std::to_string(httpCode)},
            {"detail", detail.empty() ? "-" : detail}
        });
        if (onE) {
            std::string suffix = detail.empty() ? "" : " (" + detail + ")";
            if (httpCode == 429 || httpCode >= 500) {
                onE("Qwen: service unavailable (HTTP " +
                    std::to_string(httpCode) + ")" + suffix);
            } else {
                onE("Qwen: service request failed (HTTP " +
                    std::to_string(httpCode) + ")" + suffix);
            }
        }
        return;
    }

    std::string text = qwenExtractText(respBody);

    auto tParse = std::chrono::steady_clock::now();
    fprintf(stderr, "Vinput Qwen [timer] encode=%ldms network=%ldms parse=%ldms text_len=%zu\n",
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tEncode - t0).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tNetwork - tEncode).count(),
            (long)std::chrono::duration_cast<std::chrono::milliseconds>(tParse - tNetwork).count(),
            text.size());

    if (onR && !text.empty()) {
        diagnosticLog().event("provider", "request_result", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"model", settings.model},
            {"text_length", std::to_string(text.size())},
            {"encode_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tEncode - t0).count())},
            {"network_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tNetwork - tEncode).count())},
            {"parse_ms", std::to_string(std::chrono::duration_cast<
                std::chrono::milliseconds>(tParse - tNetwork).count())}
        });
        onR(text, true);
    } else if (onE) {
        std::string detail = qwenExtractErrorDetail(respBody);
        diagnosticLog().event("provider", "request_error", {
            {"provider", "qwen"}, {"recognition_id", std::to_string(diagnosticId)},
            {"reason", "empty_result"}, {"detail", detail.empty() ? "-" : detail}
        });
        onE(detail.empty() ? "Qwen: empty result"
                           : "Qwen: empty result (" + detail + ")");
    }
}

std::unique_ptr<IAsrProvider> QwenAsrProviderFactory::create() {
    return std::make_unique<QwenAsrProvider>();
}

static struct CurlInit {
    CurlInit() { curl_global_init(CURL_GLOBAL_ALL); }
    ~CurlInit() { curl_global_cleanup(); }
} _curlInit;

static bool _qwenReg = []() {
    AsrProviderRegistry::instance().registerFactory(
        std::make_unique<QwenAsrProviderFactory>());
    return true;
}();

} // namespace vinput
