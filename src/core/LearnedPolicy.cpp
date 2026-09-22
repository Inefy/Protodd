#include "protodd/LearnedPolicy.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace protodd {
namespace {
bool u32(std::istream& input, std::uint32_t& value) {
    unsigned char bytes[4]{};
    if (!input.read(reinterpret_cast<char*>(bytes), 4)) return false;
    value = 0;
    for (unsigned i = 0; i < 4; ++i) value |= static_cast<std::uint32_t>(bytes[i]) << (i * 8);
    return true;
}
bool u64(std::istream& input, std::uint64_t& value) {
    std::uint32_t low{}, high{};
    if (!u32(input, low) || !u32(input, high)) return false;
    value = static_cast<std::uint64_t>(low) | (static_cast<std::uint64_t>(high) << 32);
    return true;
}
}  // namespace

bool LearnedPolicy::load(std::istream& input, std::string& error) {
    error.clear();
    const auto fail = [&error](const char* why) { error = why; return false; };
    char magic[8]{};
    if (!input.read(magic, 8) || std::string_view(magic, 8) != "PTDMLP1\n")
        return fail("invalid model magic");
    std::uint32_t version{}, features{}, h1{}, h2{}, actions{};
    std::uint64_t schema{};
    if (!u32(input, version) || !u64(input, schema) || !u32(input, features) ||
        !u32(input, h1) || !u32(input, h2) || !u32(input, actions)) return fail("truncated header");
    if (version != 1 || schema != macroSchemaFingerprint() ||
        features != modelFeatures().size() || actions != learnedIntents().size())
        return fail("model schema mismatch");
    if (h1 == 0 || h2 == 0 || h1 > maximumWidth || h2 > maximumWidth)
        return fail("model width out of bounds");
    const auto count = static_cast<std::size_t>(h1) * (features + 1ULL) +
        static_cast<std::size_t>(h2) * (h1 + 1ULL) + static_cast<std::size_t>(actions) * (h2 + 1ULL);
    if (count > maximumParameters) return fail("model parameter limit exceeded");
    std::vector<float> weights(static_cast<std::size_t>(count));
    for (auto& weight : weights) {
        std::uint32_t bits{};
        if (!u32(input, bits)) return fail("truncated weights");
        weight = std::bit_cast<float>(bits);
        if (!std::isfinite(weight) || std::abs(weight) > 1000.0F) return fail("invalid weight");
    }
    if (input.peek() != std::char_traits<char>::eof() || input.bad()) return fail("trailing model data");
    inputs_ = features; hidden1_ = h1; hidden2_ = h2; outputs_ = actions;
    weights_ = std::move(weights);
    return true;
}

void LearnedPolicy::clear() noexcept {
    inputs_ = hidden1_ = hidden2_ = outputs_ = 0;
    weights_.clear();
}

std::optional<LearnedPrediction> LearnedPolicy::predict(
    std::span<const float> features, LearnedActionMask allowed) const noexcept {
    if (!loaded() || features.size() != inputs_ || outputs_ == 0 || outputs_ > 64) return std::nullopt;
    if (outputs_ < 64) allowed &= (LearnedActionMask{1} << outputs_) - 1;
    if (allowed == 0) return std::nullopt;
    for (const auto feature : features)
        if (!std::isfinite(feature) || feature < 0.0F || feature > 16.0F) return std::nullopt;
    std::array<float, maximumWidth> first{}, second{};
    std::size_t offset = 0;
    const auto layer = [this, &offset](std::span<const float> source, std::span<float> target, bool relu) {
        const auto bias = offset + source.size() * target.size();
        for (std::size_t out = 0; out < target.size(); ++out) {
            float sum = weights_[bias + out];
            for (std::size_t in = 0; in < source.size(); ++in)
                sum += source[in] * weights_[offset + out * source.size() + in];
            target[out] = relu ? std::max(0.0F, sum) : sum;
        }
        offset = bias + target.size();
    };
    layer(features, std::span<float>(first.data(), hidden1_), true);
    layer(std::span<const float>(first.data(), hidden1_), std::span<float>(second.data(), hidden2_), true);
    LearnedPrediction result;
    layer(std::span<const float>(second.data(), hidden2_), std::span<float>(result.logits.data(), outputs_), false);
    float best = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < outputs_; ++i) {
        if (!std::isfinite(result.logits[i])) return std::nullopt;
        if ((allowed & (LearnedActionMask{1} << i)) && result.logits[i] > best) {
            best = result.logits[i]; result.action = i;
        }
    }
    double sum = 0;
    for (std::size_t i = 0; i < outputs_; ++i)
        if (allowed & (LearnedActionMask{1} << i)) sum += std::exp(static_cast<double>(result.logits[i] - best));
    result.probability = static_cast<float>(1.0 / sum);
    return result;
}
}  // namespace protodd
