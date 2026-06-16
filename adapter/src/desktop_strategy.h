#pragma once

#include <memory>
#include <string>

namespace vinput {

class DesktopStrategy {
public:
    virtual ~DesktopStrategy() = default;
    virtual std::string getFocusedWindowId() = 0;
    virtual void focusWindow(const std::string &id) = 0;
    virtual bool supportsSwitching() const = 0;
    virtual const char *name() const = 0;

    static std::unique_ptr<DesktopStrategy> create(const std::string &desktop);
    static std::unique_ptr<DesktopStrategy> autoDetect();
};

class NoopStrategy : public DesktopStrategy {
public:
    std::string getFocusedWindowId() override { return ""; }
    void focusWindow(const std::string &) override {}
    bool supportsSwitching() const override { return false; }
    const char *name() const override { return "none"; }
};

class NiriStrategy : public DesktopStrategy {
public:
    std::string getFocusedWindowId() override;
    void focusWindow(const std::string &id) override;
    bool supportsSwitching() const override { return true; }
    const char *name() const override { return "niri"; }
};

class HyprlandStrategy : public DesktopStrategy {
public:
    std::string getFocusedWindowId() override;
    void focusWindow(const std::string &id) override;
    bool supportsSwitching() const override { return true; }
    const char *name() const override { return "hyprland"; }
};

} // namespace vinput
