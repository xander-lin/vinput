#include "desktop_strategy.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <unistd.h>

namespace vinput {

// --- NiriStrategy ---

std::string NiriStrategy::getFocusedWindowId() {
    FILE *f = popen("niri msg focused-window 2>/dev/null", "r");
    if (!f) return "";
    char buf[256] = {};
    (void)!fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);
    const char *p = strstr(buf, "Window ID ");
    if (!p) return "";
    p += 10;
    const char *end = strchr(p, ':');
    if (!end) return "";
    return std::string(p, end - p);
}

void NiriStrategy::focusWindow(const std::string &id) {
    if (id.empty()) return;
    std::string cmd = "niri msg action focus-window --id " + id;
    pid_t pid;
    const char *argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                (char *const *)argv, environ);
}

// --- HyprlandStrategy ---

std::string HyprlandStrategy::getFocusedWindowId() {
    FILE *f = popen("hyprctl activewindow -j 2>/dev/null", "r");
    if (!f) return "";
    char buf[512] = {};
    (void)!fread(buf, 1, sizeof(buf) - 1, f);
    pclose(f);
    const char *p = strstr(buf, "\"address\"");
    if (!p) return "";
    p = strchr(p, ':');
    if (!p) return "";
    p++; // skip ':'
    while (*p == ' ' || *p == '"') p++;
    const char *end = strchr(p, '"');
    if (!end) return "";
    return std::string(p, end - p);
}

void HyprlandStrategy::focusWindow(const std::string &id) {
    if (id.empty()) return;
    std::string cmd = "hyprctl dispatch focuswindow address:" + id;
    pid_t pid;
    const char *argv[] = {"sh", "-c", cmd.c_str(), nullptr};
    posix_spawn(&pid, "/bin/sh", nullptr, nullptr,
                (char *const *)argv, environ);
}

// --- Factory ---

std::unique_ptr<DesktopStrategy> DesktopStrategy::create(const std::string &desktop) {
    if (desktop == "niri")     return std::make_unique<NiriStrategy>();
    if (desktop == "hyprland") return std::make_unique<HyprlandStrategy>();
    return std::make_unique<NoopStrategy>();
}

std::unique_ptr<DesktopStrategy> DesktopStrategy::autoDetect() {
    if (getenv("HYPRLAND_INSTANCE_SIGNATURE"))
        return std::make_unique<HyprlandStrategy>();
    if (getenv("NIRI_SOCKET"))
        return std::make_unique<NiriStrategy>();

    const char *xdg = getenv("XDG_CURRENT_DESKTOP");
    if (!xdg) xdg = getenv("XDG_SESSION_DESKTOP");
    if (xdg) {
        std::string d(xdg);
        if (d == "Hyprland" || d == "hyprland") return std::make_unique<HyprlandStrategy>();
        if (d == "niri" || d == "Niri")          return std::make_unique<NiriStrategy>();
    }

    return std::make_unique<NoopStrategy>();
}

} // namespace vinput
