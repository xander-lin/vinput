#include "vinput_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main() {
    auto base = fs::temp_directory_path() /
                ("vinput-config-test-" + std::to_string(getpid()));
    auto userDir = base / "user";
    auto systemDir = base / "system";

    writeFile(systemDir / "output.json", "{\"desktop\":\"none\"}");
    auto fallback = vinput::readConfigFileFromDirs("output.json", userDir, systemDir);
    if (vinput::jsonStr(fallback, "desktop") != "none") {
        std::cerr << "system fallback was not used\n";
        fs::remove_all(base);
        return 1;
    }
    if (!fs::exists(userDir / "output.json")) {
        std::cerr << "missing user config was not created from system config\n";
        fs::remove_all(base);
        return 1;
    }

    writeFile(userDir / "output.json", "{\"desktop\":\"niri\"}");
    auto user = vinput::readConfigFileFromDirs("output.json", userDir, systemDir);
    if (vinput::jsonStr(user, "desktop") != "niri") {
        std::cerr << "user config did not override system fallback\n";
        fs::remove_all(base);
        return 1;
    }

    writeFile(systemDir / "output.json", "{\"desktop\":\"hyprland\"}");
    auto preserved = vinput::readConfigFileFromDirs("output.json", userDir, systemDir);
    if (vinput::jsonStr(preserved, "desktop") != "niri") {
        std::cerr << "existing user config was overwritten\n";
        fs::remove_all(base);
        return 1;
    }

    if (!vinput::readConfigFileFromDirs("missing.json", userDir, systemDir).empty()) {
        std::cerr << "missing config should return empty content\n";
        fs::remove_all(base);
        return 1;
    }

    fs::remove_all(base);
    return 0;
}
