#pragma once

// Secret store for cloud provider API keys.
//
// Lifecycle contract (see config/README.md):
//   - The user writes "api_key" into the vendor JSON (qwen.json/doubao.json).
//   - On the next recognition the provider imports it into the encrypted
//     secret store and REMOVES the field from the JSON file (atomic rewrite).
//   - If the field reappears later, that means "update or set the key".
//   - When no Secret Service backend is available (headless), the import
//     fails and the plaintext field is kept so nothing is lost.
//
// The desktop backend is the freedesktop Secret Service (implemented by
// KWallet and GNOME Keyring) via the secret-tool CLI. Secrets are passed on
// stdin, never on the command line, and are never logged.

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>

#include "qwen_json.h"
#include "vinput_config.h"

namespace vinput {

class SecretStore {
public:
    virtual ~SecretStore() = default;
    // provider is the vendor id ("qwen", "doubao"). Returns true when the
    // secret was accepted by the store.
    virtual bool store(const std::string &provider, const std::string &secret) = 0;
    // Returns the stored secret or "" when absent/unavailable.
    virtual std::string lookup(const std::string &provider) = 0;
};

// Real backend: `secret-tool` talking to the Secret Service. Both calls are
// wrapped in `timeout` because a locked keyring may raise an unlock dialog.
class SecretToolStore : public SecretStore {
public:
    bool store(const std::string &provider, const std::string &secret) override {
        // --label value is fixed text plus our own provider id; single quotes
        // are safe. The secret itself travels over stdin.
        std::string cmd = "timeout 5 secret-tool store --label='Vinput " +
                          provider + " API key' service vinput provider " +
                          provider + " 2>/dev/null";
        FILE *p = popen(cmd.c_str(), "w");
        if (!p) return false;
        fwrite(secret.data(), 1, secret.size(), p);
        fflush(p);
        int status = pclose(p);
        return WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }

    std::string lookup(const std::string &provider) override {
        std::string cmd = "timeout 5 secret-tool lookup service vinput provider " +
                          provider + " 2>/dev/null";
        FILE *p = popen(cmd.c_str(), "r");
        if (!p) return "";
        std::string out;
        char buf[512];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
        int status = pclose(p);
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return "";
        // Trim trailing whitespace/newlines only.
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' ||
                                out.back() == ' ' || out.back() == '\t')) {
            out.pop_back();
        }
        return out;
    }
};

// Mock backend for tests and environments without a Secret Service: never
// stores, never returns anything. Providers fall back to the JSON field.
class NullSecretStore : public SecretStore {
public:
    bool store(const std::string &, const std::string &) override { return false; }
    std::string lookup(const std::string &) override { return ""; }
};

// Active store (pluggable; tests inject NullSecretStore or a capturing fake).
inline std::unique_ptr<SecretStore> &secretStorePtr() {
    static std::unique_ptr<SecretStore> store = std::make_unique<SecretToolStore>();
    return store;
}

inline SecretStore &activeSecretStore() { return *secretStorePtr(); }

inline void setActiveSecretStore(std::unique_ptr<SecretStore> store) {
    // Not thread-safe by design: call once at startup or in test mains
    // before providers exist.
    secretStorePtr() = std::move(store);
}

// Best-effort import of a plaintext config key into the secret store,
// followed by stripping the field from the user's config file (atomic
// rewrite, file mode 0600). Returns true when the file no longer contains
// the plaintext key. Warns at most once per provider per process when no
// Secret Service backend is available; the plaintext field is then kept so
// hands-off operation degrades gracefully instead of losing the key.
inline bool importConfigSecret(const std::string &provider,
                               const std::string &configFile,
                               const std::string &secret) {
    if (!activeSecretStore().store(provider, secret)) {
        static std::set<std::string> warned;
        if (warned.insert(provider).second) {
            fprintf(stderr,
                    "Vinput %s: secret store unavailable (no KWallet/GNOME "
                    "Keyring?), keeping api_key in %s\n",
                    provider.c_str(), configFile.c_str());
        }
        return false;
    }
    std::string path = configPath(configFile);
    std::string orig = readFileIfExists(path);
    size_t root = orig.find('{');
    if (qjsonTopLevelString(orig, root, "api_key").empty()) {
        // Already stripped (import succeeded on an earlier run, or the caller
        // passed a key parsed from a different copy): the store is now
        // authoritative, nothing to rewrite. Nested api_key keys (e.g. inside
        // vocabulary) do not count.
        return true;
    }
    std::string stripped = qjsonRemoveTopLevelProperty(orig, "api_key");
    if (stripped.empty()) {
        static std::set<std::string> rewriteWarned;
        if (rewriteWarned.insert(provider).second) {
            fprintf(stderr,
                    "Vinput %s: api_key imported into secret store, but %s "
                    "could not be rewritten; plaintext field kept (it will "
                    "re-import next run)\n",
                    provider.c_str(), path.c_str());
        }
        return false;
    }
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) {
            fprintf(stderr, "Vinput %s: cannot write %s\n",
                    provider.c_str(), tmp.c_str());
            return false;
        }
        f << stripped;
    }
    std::error_code ec;
    std::filesystem::permissions(
        tmp,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        fprintf(stderr, "Vinput %s: cannot replace %s (%s)\n",
                provider.c_str(), path.c_str(), ec.message().c_str());
        unlink(tmp.c_str());
        return false;
    }
    fprintf(stderr,
            "Vinput %s: api_key imported into secret store and removed from %s\n",
            provider.c_str(), path.c_str());
    return true;
}

// Key lifecycle resolution used by cloud providers on every recognition:
//   override (setConfig) > plaintext api_key in the vendor JSON (import
//   trigger) > keyring (cached in the caller-owned string).
// The JSON file is checked each time so a reappearing field means
// "update the key".
inline std::string resolveApiKeyLifecycle(const std::string &provider,
                                          const std::string &configFile,
                                          const std::string &fileKey,
                                          std::string &keyringCache,
                                          const std::string &overrideKey = "") {
    if (!overrideKey.empty()) return overrideKey;
    if (!fileKey.empty()) {
        if (importConfigSecret(provider, configFile, fileKey))
            keyringCache = fileKey;  // avoid a lookup next time
        return fileKey;
    }
    if (keyringCache.empty()) keyringCache = activeSecretStore().lookup(provider);
    return keyringCache;
}

} // namespace vinput
