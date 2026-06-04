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

private:
    void recordLoop();
    static void normalizeAndWriteWav(std::vector<int16_t> &samples,
                                      const std::string &path);

    std::thread recordThread_;
    std::atomic<bool> stopRequested_{false};
    std::mutex sampleMutex_;
    std::vector<int16_t> samples_;
    std::string wavPath_;
    size_t bufferBytes_{0};

    StateCallback onState_;
    StatusTextCallback onStatusText_;
};

} // namespace vinput
