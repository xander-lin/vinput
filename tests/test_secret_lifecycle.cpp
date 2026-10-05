#include "secret_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string &what) {
    if (!ok) {
        failures++;
        std::cerr << "FAIL: " << what << "\n";
    }
}

struct FakeSecretStore : public vinput::SecretStore {
    std::map<std::string, std::string> stored;
    bool failStore = false;

    bool store(const std::string &provider, const std::string &secret) override {
        if (failStore) return false;
        stored[provider] = secret;
        return true;
    }

    std::string lookup(const std::string &provider) override {
        auto it = stored.find(provider);
        return it == stored.end() ? "" : it->second;
    }
};

std::string readFile(const std::filesystem::path &p) {
    std::ifstream f(p);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

void writeFile(const std::filesystem::path &p, const std::string &content) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream f(p);
    f << content;
}

// A plaintext api_key in the vendor JSON is imported into the secret store
// and stripped from the file; everything else is preserved.
void testImportStripsKey(FakeSecretStore *fake,
                         const std::filesystem::path &home) {
    auto cfg = home / ".config/vinput/qwen.json";
    writeFile(cfg,
              "{\n"
              "    \"api_key\": \"sk-test-123\",\n"
              "    \"model\": \"qwen-audio-3.1-asr-flash\",\n"
              "    \"vocabulary\": {\"api_key\": \"nested-must-stay\"}\n"
              "}\n");

    bool ok = vinput::importConfigSecret("qwen", "qwen.json", "sk-test-123");
    check(ok, "importConfigSecret succeeds with working store");
    check(fake->stored["qwen"] == "sk-test-123",
          "secret reached the store");

    std::string after = readFile(cfg);
    check(after.find("sk-test-123") == std::string::npos,
          "plaintext key removed from file");
    size_t occurrences = 0;
    for (size_t p = after.find("api_key"); p != std::string::npos;
         p = after.find("api_key", p + 1)) {
        occurrences++;
    }
    check(occurrences == 1, "only the nested api_key remains");
    check(after.find("nested-must-stay") != std::string::npos,
          "nested api_key inside vocabulary kept");
    check(after.find("qwen-audio-3.1-asr-flash") != std::string::npos,
          "other fields preserved");
    // Rewritten file must still parse to the expected model.
    check(vinput::qjsonStringValue(after, "model") ==
          "qwen-audio-3.1-asr-flash", "rewritten JSON stays valid");

    using vinput::qjsonRawValue;
    check(qjsonRawValue(after, "vocabulary") ==
          "{\"api_key\": \"nested-must-stay\"}",
          "vocabulary object intact after strip");

    auto perms = std::filesystem::status(cfg).permissions();
    check((perms & std::filesystem::perms::group_all) ==
              std::filesystem::perms::none &&
              (perms & std::filesystem::perms::others_all) ==
              std::filesystem::perms::none,
          "rewritten config is 0600");
}

// Without a Secret Service backend the plaintext key must survive so the
// provider still works and nothing is lost.
void testFailedImportKeepsKey(FakeSecretStore *fake,
                              const std::filesystem::path &home) {
    auto cfg = home / ".config/vinput/doubao.json";
    writeFile(cfg, "{\"api_key\":\"dk-1\",\"resource_id\":\"volc.seedasr.auc\"}");
    fake->failStore = true;

    bool ok = vinput::importConfigSecret("doubao", "doubao.json", "dk-1");
    check(!ok, "importConfigSecret reports failure");
    std::string after = readFile(cfg);
    check(after.find("dk-1") != std::string::npos,
          "plaintext key kept when store unavailable");
    fake->failStore = false;
}

void testLifecycleResolution(FakeSecretStore *fake) {
    // Override (setConfig) wins over everything.
    std::string cache;
    check(vinput::resolveApiKeyLifecycle("qwen", "qwen.json", "file-key",
                                         cache, "override-key") == "override-key",
          "override key wins");

    // fileKey triggers import and is returned.
    cache.clear();
    check(vinput::resolveApiKeyLifecycle("qwen", "qwen.json", "file-key",
                                         cache) == "file-key",
          "file key returned while importing");
    check(fake->stored["qwen"] == "file-key", "file key imported");
    check(cache == "file-key", "keyring cache primed after import");

    // Without fileKey the key comes from the keyring (via cache).
    check(vinput::resolveApiKeyLifecycle("qwen", "qwen.json", "", cache) ==
          "file-key", "keyring cache reused");
    cache.clear();
    check(vinput::resolveApiKeyLifecycle("qwen", "qwen.json", "", cache) ==
          "file-key", "keyring lookup after cache reset");

    // Nothing available -> empty.
    cache.clear();
    check(vinput::resolveApiKeyLifecycle("doubao", "doubao.json", "", cache)
              .empty(),
          "no override/file/keyring yields empty key");
}

} // namespace

int main() {
    const char *tmp = std::getenv("MESON_TEST_TMPDIR");
    std::filesystem::path root =
        (tmp && *tmp) ? tmp : "/tmp/vinput-secret-lifecycle";
    std::filesystem::path home = root / "home";
    std::filesystem::create_directories(home / ".config/vinput");
    setenv("HOME", home.c_str(), 1);

    auto fake = new FakeSecretStore();
    vinput::setActiveSecretStore(std::unique_ptr<vinput::SecretStore>(fake));

    testImportStripsKey(fake, home);
    testFailedImportKeepsKey(fake, home);
    testLifecycleResolution(fake);

    if (failures) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "secret_lifecycle: all checks passed\n";
    return 0;
}
