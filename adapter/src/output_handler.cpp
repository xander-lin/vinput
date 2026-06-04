#include "output_handler.h"
#include "desktop_strategy.h"

#include <fcitx/inputcontextmanager.h>
#include <fcitx-utils/log.h>

#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <fstream>

namespace vinput {

static std::string readDesktopConfig() {
    const char *home = getenv("HOME");
    if (!home) return "none";
    std::string path = std::string(home) + "/.config/vinput/output.json";
    std::ifstream f(path);
    if (!f) return "none";
    std::string json((std::istreambuf_iterator<char>(f)),
                      std::istreambuf_iterator<char>());
    auto pos = json.find("\"desktop\"");
    if (pos == std::string::npos) return "none";
    pos = json.find('"', json.find(':', pos) + 1);
    if (pos == std::string::npos) return "none";
    pos++;
    auto end = json.find('"', pos);
    if (end == std::string::npos) return "none";
    return json.substr(pos, end - pos);
}

OutputHandler::OutputHandler(fcitx::Instance *instance) : instance_(instance) {
    auto desktop = readDesktopConfig();
    desktop_ = DesktopStrategy::create(desktop);
    FCITX_INFO() << "Vinput OutputHandler: desktop=" << desktop_
                 << " strategy=" << desktop_->name();

    if (pipe(wakePipe_) != 0) {
        FCITX_ERROR() << "Vinput: OutputHandler pipe() failed";
        return;
    }
    fcntl(wakePipe_[0], F_SETFL, O_NONBLOCK);
    fcntl(wakePipe_[1], F_SETFL, O_NONBLOCK);

    wakeWatcher_ = instance_->eventLoop().addIOEvent(
        wakePipe_[0], fcitx::IOEventFlag::In,
        [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) -> bool {
            drainAndCommit();
            return true;
        });
}

OutputHandler::~OutputHandler() {
    if (wakePipe_[0] >= 0) close(wakePipe_[0]);
    if (wakePipe_[1] >= 0) close(wakePipe_[1]);
}

void OutputHandler::submit(const std::string &text) {
    enqueue(text, false);
}

void OutputHandler::showStatus(const std::string &text) {
    enqueue(text, true);
}

void OutputHandler::setCaptureWindow(const std::string &winId) {
    capturedWinId_ = winId;
}

void OutputHandler::captureCurrentWindow() {
    capturedWinId_ = desktop_->getFocusedWindowId();
}

void OutputHandler::clearCaptureWindow() {
    capturedWinId_.clear();
}

void OutputHandler::setPressTime(std::chrono::steady_clock::time_point t) {
    tPress_ = t;
}

void OutputHandler::enqueue(const std::string &text, bool isStatus) {
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        pending_.push_back({text, isStatus});
    }
    char c = 1;
    (void)!write(wakePipe_[1], &c, 1);
}

void OutputHandler::drainAndCommit() {
    char buf[64];
    while (read(wakePipe_[0], buf, sizeof(buf)) > 0) {}

    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lk(pendingMutex_);
        batch.swap(pending_);
    }

    // Status texts: directly commit to current IC
    for (auto &p : batch) {
        if (!p.isStatus) continue;
        auto *ic = instance_->mostRecentInputContext();
        if (ic && !p.text.empty()) ic->commitString(p.text);
    }

    // Check if any non-status items need committing
    bool hasCommit = false;
    for (auto &p : batch) {
        if (!p.isStatus) { hasCommit = true; break; }
    }
    if (!hasCommit) return;

    // No captured window or strategy doesn't support switching → direct commit
    if (capturedWinId_.empty() || !desktop_->supportsSwitching()) {
        commitBatch(batch, "commit");
        return;
    }

    auto restoreId = desktop_->getFocusedWindowId();
    if (restoreId.empty()) {
        FCITX_INFO() << "Vinput [" << desktop_->name() << "] failed to get focused window, direct commit";
        commitBatch(batch, "commit");
        return;
    }

    if (restoreId == capturedWinId_) {
        FCITX_INFO() << "Vinput [" << desktop_->name() << "] window unchanged, direct commit";
        commitBatch(batch, "commit");
        return;
    }

    auto capturedId = capturedWinId_;
    FCITX_INFO() << "Vinput [" << desktop_->name() << "] captured=" << capturedId
                 << " restore=" << restoreId;

    desktop_->focusWindow(capturedId);

    pendingNiriBatch_ = std::move(batch);
    pendingRestoreId_ = restoreId;
    niriRetryCount_ = 0;

    niriPollTimer_ = instance_->eventLoop().addTimeEvent(
        CLOCK_MONOTONIC,
        fcitx::now(CLOCK_MONOTONIC) + kNiriRetryIntervalUsec, kNiriRetryIntervalUsec,
        [this](fcitx::EventSourceTime *, uint64_t) -> bool {
            return niriPollTick();
        });
}

bool OutputHandler::niriPollTick() {
    niriRetryCount_++;
    auto cur = desktop_->getFocusedWindowId();

    if (cur == capturedWinId_ || niriRetryCount_ >= kNiriRetryMax) {
        if (cur != capturedWinId_) {
            fprintf(stderr, "Vinput [%s] focus switch timeout after %dms, committing anyway\n",
                    desktop_->name(), niriRetryCount_ * 10);
        }

        commitBatch(pendingNiriBatch_, "commit");

        if (!pendingRestoreId_.empty() && pendingRestoreId_ != capturedWinId_) {
            desktop_->focusWindow(pendingRestoreId_);
        }

        pendingNiriBatch_.clear();
        return false;
    }

    return true;
}

void OutputHandler::commitBatch(const std::vector<Pending> &batch, const char *label) {
    auto tNow = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tNow - tPress_).count();
    fprintf(stderr, "Vinput [timer] press→%s=%ldms\n", label, ms);
    for (auto &p : batch) {
        if (p.isStatus) continue;
        auto *ic = instance_->mostRecentInputContext();
        if (!ic) {
            FCITX_INFO() << "Vinput [" << label << "] no focused ic, drop";
            continue;
        }
        FCITX_INFO() << "Vinput [" << label << "] ic=" << ic
                     << " program=" << ic->program()
                     << " text=\"" << p.text << "\"";
        if (!p.text.empty()) ic->commitString(p.text);
    }
}

} // namespace vinput
