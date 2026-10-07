#include "vinput_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

static int failures = 0;

static void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

static void writeFile(const fs::path &path, const std::string &content) {
    fs::create_directories(path.parent_path());
    std::ofstream f(path);
    f << content;
}

int main() {
    auto base = fs::temp_directory_path() /
                ("vinput-config-read-" + std::to_string(getpid()));
    auto home = base / "home";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);

    // Missing file: empty content, defaults apply.
    check(vinput::readConfigFile("config.json").empty(),
          "missing config reads as empty");

    writeFile(home / ".config/vinput/config.json", R"JSON({
    // which provider is active; Ctrl+CapsLock rewrites this line
    "provider": "qwen",
    "ui": { "activation_msec": 250 },   // faster hold activation
    "audio": {
        "denoise": "speexdsp",
        "lufs_target": -16.0
    }
})JSON");

    auto cfg = vinput::readConfigFile("config.json");
    check(vinput::jsonStr(cfg, "provider") == "qwen",
          "provider parsed from commented file");

    // Comments are stripped as spaces: offsets still match the original file.
    check(cfg.size() == fs::file_size(home / ".config/vinput/config.json"),
          "stripping preserves length (byte offsets stay valid)");

    // '/' inside strings must never be treated as a comment.
    writeFile(home / ".config/vinput/qwen.json",
              "{\"api_key\":\"sk-x\", \"endpoint\":\"https://dashscope.aliyuncs.com/x\"}");
    auto qwen = vinput::readConfigFile("qwen.json");
    check(vinput::jsonStr(qwen, "endpoint") == "https://dashscope.aliyuncs.com/x",
          "URL inside string survives comment stripping");

    // Section extraction.
    auto audio = vinput::readConfigSection("config.json", "audio");
    check(vinput::jsonStr(audio, "denoise") == "speexdsp",
          "audio section extracted");
    auto ui = vinput::readConfigSection("config.json", "ui");
    check(vinput::jsonInt(ui, "activation_msec") == 250,
          "ui section extracted");
    check(vinput::readConfigSection("config.json", "missing").empty(),
          "absent section reads as empty");
    check(vinput::readConfigSection("nope.json", "audio").empty(),
          "absent file reads as empty section");

    fs::remove_all(base);
    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "config_read: all checks passed\n";
    return 0;
}
