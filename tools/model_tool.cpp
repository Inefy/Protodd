#include "protodd/LearnedPolicy.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

namespace {
void schema() {
    using namespace protodd;
    std::cout << "{\"version\":\"" << macroSchemaVersion << "\",\"fingerprint\":\""
              << std::hex << macroSchemaFingerprint() << std::dec << "\",\"clip\":[0,16],\"features\":[";
    bool first = true;
    for (const auto& feature : modelFeatures()) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"name\":\"" << feature.name << "\",\"scale\":" << feature.scale << '}';
    }
    std::cout << "],\"actions\":[";
    first = true;
    for (const auto& intent : learnedIntents()) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << intent.name << '"';
    }
    std::cout << "]}\n";
}
}  // namespace

int main(int argc, char** argv) {
    using namespace protodd;
    if (argc == 2 && std::string_view(argv[1]) == "schema") { schema(); return 0; }
    if (argc != 3 || (std::string_view(argv[1]) != "predict" && std::string_view(argv[1]) != "benchmark")) {
        std::cerr << "Usage: model_tool schema | model_tool predict MODEL.bin | model_tool benchmark MODEL.bin\n"
                     "predict stdin: one decimal mask followed by encoded features per line\n";
        return 2;
    }
    LearnedPolicy policy;
    std::ifstream input(argv[2], std::ios::binary);
    std::string error;
    if (!policy.load(input, error)) { std::cerr << error << '\n'; return 1; }
    std::cout << std::setprecision(9);
    if (std::string_view(argv[1]) == "benchmark") {
        std::vector<float> features(modelFeatures().size(), 0.2F);
        std::vector<double> timings;
        float checksum = 0;
        for (int i = 0; i < 276; ++i) {
            const auto start = std::chrono::steady_clock::now();
            const auto result = policy.predict(features, ~LearnedActionMask{0});
            const auto elapsed = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            if (!result) return 1;
            checksum += result->probability;
            if (i >= 20) timings.push_back(elapsed);
        }
        std::ranges::sort(timings);
        std::cout << "{\"iterations\":256,\"parameters\":" << policy.parameterCount()
                  << ",\"p50_ms\":" << timings[128] << ",\"p95_ms\":" << timings[243]
                  << ",\"p99_ms\":" << timings[253] << ",\"max_ms\":" << timings.back()
                  << ",\"checksum\":" << checksum << "}\n";
        return 0;
    }
    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream row(line);
        LearnedActionMask mask{};
        std::vector<float> features(modelFeatures().size());
        if (!(row >> mask)) { std::cerr << "invalid mask\n"; return 1; }
        for (auto& feature : features)
            if (!(row >> feature)) { std::cerr << "invalid features\n"; return 1; }
        std::string extra;
        if (row >> extra) { std::cerr << "trailing features\n"; return 1; }
        const auto prediction = policy.predict(features, mask);
        if (!prediction) { std::cerr << "prediction rejected\n"; return 1; }
        std::cout << "{\"action\":" << prediction->action << ",\"probability\":"
                  << prediction->probability << ",\"logits\":[";
        for (std::size_t i = 0; i < learnedIntents().size(); ++i) {
            if (i > 0) std::cout << ',';
            std::cout << prediction->logits[i];
        }
        std::cout << "]}\n";
    }
    return 0;
}
