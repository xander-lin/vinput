#pragma once

#include <fcitx/inputcontext.h>
#include <fcitx/instance.h>
#include <fcitx-utils/event.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vinput {

class DesktopStrategy;

class OutputHandler {
public:
    explicit OutputHandler(fcitx::Instance *instance);
    ~OutputHandler();

    OutputHandler(const OutputHandler &) = delete;
    OutputHandler &operator=(const OutputHandler &) = delete;

    // Thread-safe: submit ASR result text for display
    void submit(const std::string &text);

    // Thread-safe: show transient status text
    void showStatus(const std::string &text);

    // Capture current focused window via desktop strategy
    void captureCurrentWindow();

    // niri window focus
    void setCaptureWindow(const std::string &winId);
    void clearCaptureWindow();

    // Timing reference for diagnostics
    void setPressTime(std::chrono::steady_clock::time_point t);

private:
    fcitx::Instance *instance_;

    int wakePipe_[2] = {-1, -1};
    std::unique_ptr<fcitx::EventSourceIO> wakeWatcher_;

    struct Pending {
        std::string text;
        bool isStatus = false;
    };
    std::mutex pendingMutex_;
    std::vector<Pending> pending_;

    std::unique_ptr<DesktopStrategy> desktop_;
    std::string capturedWinId_;
    std::chrono::steady_clock::time_point tPress_;

    // niri async focus polling
    std::vector<Pending> pendingNiriBatch_;
    std::string pendingRestoreId_;
    int niriRetryCount_ = 0;
    std::unique_ptr<fcitx::EventSourceTime> niriPollTimer_;

    static constexpr int kNiriRetryMax = 50;
    static constexpr int kNiriRetryIntervalUsec = 10000;

    void enqueue(const std::string &text, bool isStatus);
    void drainAndCommit();
    void commitBatch(const std::vector<Pending> &batch, const char *label);
    bool niriPollTick();
};

} // namespace vinput
