#include "WholeGameIntent.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace protodd::cpu {
namespace {

const std::vector<float>& head(const Output& prediction, std::string_view name,
                               std::size_t expected = 0) {
    const auto found = prediction.heads.find(std::string(name));
    if (found == prediction.heads.end() || found->second.empty() ||
        (expected && found->second.size() != expected) ||
        !std::all_of(found->second.begin(), found->second.end(),
                     [](float value) { return std::isfinite(value); }))
        throw std::runtime_error("invalid whole-game intent head: " + std::string(name));
    return found->second;
}

std::size_t top(const std::vector<float>& values) {
    return static_cast<std::size_t>(std::max_element(values.begin(), values.end()) - values.begin());
}

float sigmoid(float value) { return 1.0f / (1.0f + std::exp(-value)); }

float topProbability(const std::vector<float>& logits, std::size_t selected) {
    const float maximum = *std::max_element(logits.begin(), logits.end());
    float total = 0;
    for (const auto value : logits) total += std::exp(value - maximum);
    return std::exp(logits[selected] - maximum) / total;
}

}  // namespace

std::optional<Intent> decodeIntent(const Output& prediction,
                                   const EncodedObservation& encoded,
                                   const whole_observation::Snapshot& source,
                                   const IntentThresholds& thresholds) {
    if (thresholds.eventProbability < 0 || thresholds.eventProbability > 1 ||
        thresholds.maxActors == 0 || !std::isfinite(thresholds.actorLogit))
        throw std::invalid_argument("invalid whole-game intent thresholds");
    const auto count = encoded.entityIds.size();
    if (!count || encoded.input.relation.size() != count ||
        source.entities.empty() || encoded.input.width == 0 || encoded.input.height == 0)
        throw std::runtime_error("invalid whole-game intent observation");
    const auto event = sigmoid(head(prediction, "event", 1)[0]);
    if (event < thresholds.eventProbability) return std::nullopt;
    const auto& kind = head(prediction, "kind", kindNames.size());
    const auto& modes = head(prediction, "target_mode", targetModeNames.size());
    const auto& actor = head(prediction, "actor", count);
    const auto& target = head(prediction, "target", count);
    Intent result;
    float bestPair = -std::numeric_limits<float>::infinity();
    for (std::size_t candidate = 0; candidate < kind.size(); ++candidate)
        for (std::size_t mode = 0; mode < modes.size(); ++mode)
            if ((kindTargetModeMask[candidate] & (1u << mode)) != 0 &&
                kind[candidate] + modes[mode] > bestPair) {
                bestPair = kind[candidate] + modes[mode];
                result.kind = candidate;
                result.targetMode = mode;
            }
    result.kindProbability = topProbability(kind, result.kind);
    result.eventProbability = event;
    result.domain = top(head(prediction, "domain", domainNames.size()));
    result.queued = top(head(prediction, "queued", 2)) == 1;
    result.unitType = static_cast<int>(top(head(prediction, "unit_type", 256)));
    result.technology = static_cast<int>(top(head(prediction, "technology", 44)));
    result.upgrade = static_cast<int>(top(head(prediction, "upgrade", 61)));
    result.queueSlot = static_cast<int>(top(head(prediction, "queue_slot", 16)));
    std::vector<std::size_t> actorIndexes;
    for (std::size_t index = 0; index < count; ++index)
        if (encoded.input.relation[index] == 0 && actor[index] >= thresholds.actorLogit)
            actorIndexes.push_back(index);
    std::sort(actorIndexes.begin(), actorIndexes.end(), [&](std::size_t left, std::size_t right) {
        return actor[left] == actor[right] ? encoded.entityIds[left] < encoded.entityIds[right]
                                           : actor[left] > actor[right];
    });
    for (std::size_t index = 0; index < std::min(thresholds.maxActors, actorIndexes.size()); ++index)
        result.actorIds.push_back(encoded.entityIds[actorIndexes[index]]);
    if (result.actorIds.empty()) return std::nullopt;
    if (result.targetMode == 1) {
        std::optional<std::size_t> selected;
        for (std::size_t index = 0; index < count; ++index) {
            const auto entity = source.entities.find(encoded.entityIds[index]);
            if (entity == source.entities.end() || !entity->second.visible) continue;
            if (!selected || target[index] > target[*selected]) selected = index;
        }
        if (!selected) return std::nullopt;
        result.targetEntityId = encoded.entityIds[*selected];
    } else if (result.targetMode == 2) {
        const auto& position = head(prediction, "position");
        if (position.size() % 5 != 0) throw std::runtime_error("invalid position mixture shape");
        std::size_t selected = 0;
        for (std::size_t index = 1; index < position.size() / 5; ++index)
            if (position[index * 5] > position[selected * 5]) selected = index;
        const auto pixelsWide = static_cast<int>(encoded.input.width * 32);
        const auto pixelsHigh = static_cast<int>(encoded.input.height * 32);
        const auto x = std::clamp(static_cast<int>(sigmoid(position[selected * 5 + 1]) * pixelsWide),
                                  0, pixelsWide - 1);
        const auto y = std::clamp(static_cast<int>(sigmoid(position[selected * 5 + 2]) * pixelsHigh),
                                  0, pixelsHigh - 1);
        result.targetPixel = std::pair{x, y};
    }
    return result;
}

}  // namespace protodd::cpu
