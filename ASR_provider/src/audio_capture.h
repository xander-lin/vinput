#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cstdint>
#include <functional>

namespace vinput {

class AudioCapture {
public:
    using StateCallback = std::function<void(bool active)>;
    using StatusTextCallback = std::function<void(const std::string &)>;

    AudioCapture();
    ~AudioCapture();

    void start();
    void stop();

    bool recording() const { return recordThread_.joinable() && !stopRequested_; }

    std::vector<int16_t> takeSamples();
    const std::string& wavPath() const { return wavPath_; }

    void setStateCallback(StateCallback cb) { onState_ = std::move(cb); }
    void setStatusTextCallback(StatusTextCallback cb) { onStatusText_ = std::move(cb); }

    void setDenoiseEnabled(bool enabled) { denoiseEnabled_ = enabled; }

private:
    void recordLoop();
    void applyDenoise(std::vector<int16_t> &samples);
    static double normalizeSamples(std::vector<int16_t> &samples);
    static bool detectBlank(const std::vector<int16_t> &samples, double eburLoudness);
    static void writeWav(const std::vector<int16_t> &samples, const std::string &path);

    std::thread recordThread_;
    std::atomic<bool> stopRequested_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::string wavPath_;
    size_t bufferBytes_{0};
    bool denoiseEnabled_{false};

    StateCallback onState_;
    StatusTextCallback onStatusText_;
};

} // namespace vinput
