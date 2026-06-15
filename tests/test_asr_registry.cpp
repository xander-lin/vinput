#include "asr_provider.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main() {
    auto &registry = vinput::AsrProviderRegistry::instance();
    auto factories = registry.listFactories();

    bool foundMock = false;
    for (const auto &factory : factories) {
        if (factory.first == "mock") {
            foundMock = true;
            break;
        }
    }

    if (!foundMock) {
        std::cerr << "mock ASR provider is not registered\n";
        return 1;
    }

    auto provider = registry.create("mock");
    if (!provider) {
        std::cerr << "failed to create mock ASR provider\n";
        return 1;
    }

    std::string result;
    bool final = false;
    provider->setResultCallback([&](const std::string &text, bool isFinal) {
        result = text;
        final = isFinal;
    });

    provider->transcribe(std::vector<int16_t>{1, -1, 2, -2}, "");

    if (result != "hello world" || !final) {
        std::cerr << "unexpected mock result: text='" << result
                  << "' final=" << final << "\n";
        return 1;
    }

    return 0;
}
