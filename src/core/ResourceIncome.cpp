#include "protodd/ResourceIncome.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace protodd {
namespace {

constexpr Frame sampleIntervalFrames = 24;
constexpr Frame minimumRateWindowFrames = 12 * 24;
constexpr std::int64_t framesPerMinute = 60 * 24;

int perMinute(const int delta, const Frame elapsed) noexcept {
    if (delta < 0 || elapsed <= 0) return -1;
    const auto scaled = static_cast<std::int64_t>(delta) * framesPerMinute + elapsed / 2;
    return static_cast<int>(std::clamp<std::int64_t>(
        scaled / elapsed, 0, std::numeric_limits<int>::max()));
}

}  // namespace

void ResourceIncomeTracker::reset() noexcept {
    samples_ = {};
    sampleCount_ = 0;
    nextSample_ = 0;
    lastObservedFrame_ = -1;
}

void ResourceIncomeTracker::append(const Sample sample) noexcept {
    samples_[nextSample_] = sample;
    nextSample_ = (nextSample_ + 1) % sampleCapacity;
    sampleCount_ = std::min(sampleCount_ + 1, sampleCapacity);
}

ResourceIncomeEstimate ResourceIncomeTracker::update(
    const Frame frame,
    const int gatheredMinerals,
    const int gatheredGas) noexcept {
    if (frame < 0 || gatheredMinerals < 0 || gatheredGas < 0) return {};

    const auto newestIndex = sampleCount_ == 0
        ? 0 : (nextSample_ + sampleCapacity - 1) % sampleCapacity;
    const auto resetWindow = sampleCount_ > 0 &&
        (frame < lastObservedFrame_ ||
         gatheredMinerals < samples_[newestIndex].minerals ||
         gatheredGas < samples_[newestIndex].gas);
    if (resetWindow) reset();

    const auto current = Sample{frame, gatheredMinerals, gatheredGas};
    if (sampleCount_ == 0) {
        append(current);
    } else if (frame - samples_[(nextSample_ + sampleCapacity - 1) % sampleCapacity].frame >=
               sampleIntervalFrames) {
        append(current);
    }
    lastObservedFrame_ = frame;

    if (sampleCount_ < 2) return {};
    const auto oldestIndex = (nextSample_ + sampleCapacity - sampleCount_) % sampleCapacity;
    const auto latestIndex = (nextSample_ + sampleCapacity - 1) % sampleCapacity;
    const auto& oldest = samples_[oldestIndex];
    const auto& latest = samples_[latestIndex];
    const auto elapsed = latest.frame - oldest.frame;
    if (elapsed < minimumRateWindowFrames) return {};

    return {perMinute(latest.minerals - oldest.minerals, elapsed),
            perMinute(latest.gas - oldest.gas, elapsed)};
}

}  // namespace protodd
