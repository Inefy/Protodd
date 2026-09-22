#include "protodd/ObservationEncoder.hpp"

#include "protodd/Technology.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace protodd {
namespace {
constexpr auto firstUnit = static_cast<std::size_t>(UnitKind::probe);
constexpr auto unitCount = static_cast<std::size_t>(UnitKind::count);
constexpr auto techCount = static_cast<std::size_t>(TechnologyKind::count);

bool completed(const GameState& state, UnitKind kind) {
    return std::ranges::any_of(state.self.units, [kind](const auto& unit) {
        return unit.kind == kind && unit.completed && !unit.hallucination;
    });
}
std::string unitName(UnitKind kind) {
    std::string result(unitStats(kind).name);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == ' ') c = '_';
    }
    return result;
}
std::string technologyName(TechnologyKind kind) {
    std::string result(technologyStats(kind).name);
    for (auto& c : result) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c == ' ') c = '_';
    }
    return result;
}
}  // namespace

const std::vector<ModelFeature>& modelFeatures() {
    static const auto features = [] {
        std::vector<ModelFeature> result{
            {"frame", 86400}, {"minerals", 2000}, {"gas", 2000},
            {"supply_used_doubled", 400}, {"supply_total_doubled", 400},
            {"gathered_minerals", 20000}, {"gathered_gas", 10000},
            {"map_width_pixels", 4096}, {"map_height_pixels", 4096},
            {"enemy_protoss", 1},
            {"enemy_terran", 1}, {"enemy_zerg", 1}, {"enemy_unknown", 1},
            {"gas_workers", 24},
            {"history_ready", 1}, {"history_elapsed_frames", 240},
            {"mineral_income_per_second", 100}, {"gas_income_per_second", 100}
        };
        // Append-only UnitKind values are converted to a named, fingerprinted
        // contract. Changing names, order, scales or actions invalidates weights.
        for (std::size_t i = firstUnit; i < unitCount; ++i) {
            const auto name = unitName(static_cast<UnitKind>(i));
            for (const auto* prefix : {"own_complete/", "own_incomplete/", "own_queue/",
                                       "enemy_visible/", "enemy_remembered/"})
                result.push_back({std::string(prefix) + name, 50});
            result.push_back({"enemy_memory_age/" + name, 240 * 24});
        }
        for (std::size_t i = 1; i < techCount; ++i) {
            const auto name = technologyName(static_cast<TechnologyKind>(i));
            result.push_back({"own_tech_level/" + name, 3});
            result.push_back({"own_tech_in_progress/" + name, 1});
        }
        return result;
    }();
    return features;
}

const std::vector<LearnedIntent>& learnedIntents() {
    static const auto intents = [] {
        std::vector<LearnedIntent> result{{"wait", LearnedIntentKind::wait}};
        for (std::size_t i = firstUnit; i <= static_cast<std::size_t>(UnitKind::arbiter); ++i) {
            const auto kind = static_cast<UnitKind>(i);
            // Merges need a pair-selection executor and separate labels.
            if (kind == UnitKind::archon || kind == UnitKind::darkArchon) continue;
            const auto action = kind == UnitKind::nexus ? LearnedIntentKind::expand :
                isBuilding(kind) ? LearnedIntentKind::build : LearnedIntentKind::train;
            const auto prefix = kind == UnitKind::nexus ? "expand_" :
                isBuilding(kind) ? "build_" : "train_";
            result.push_back({std::string(prefix) + unitName(kind), action, kind});
        }
        for (std::size_t i = 1; i < techCount; ++i) {
            const auto kind = static_cast<TechnologyKind>(i);
            const bool research = technologyStats(kind).research;
            result.push_back({std::string(research ? "research_" : "upgrade_") + technologyName(kind),
                              research ? LearnedIntentKind::research : LearnedIntentKind::upgrade,
                              UnitKind::unknown, kind});
        }
        if (result.size() > 64) throw std::logic_error("Learned action mask overflow");
        return result;
    }();
    return intents;
}

std::uint64_t macroSchemaFingerprint() {
    static const auto fingerprint = [] {
        std::uint64_t hash = UINT64_C(14695981039346656037);
        const auto byte = [&hash](unsigned char value) { hash ^= value; hash *= UINT64_C(1099511628211); };
        const auto text = [&byte](std::string_view value) {
            for (const auto c : value) byte(static_cast<unsigned char>(c));
            byte(0);
        };
        text(macroSchemaVersion);
        for (const auto& feature : modelFeatures()) {
            text(feature.name);
            const auto bits = std::bit_cast<std::uint32_t>(feature.scale);
            for (unsigned shift = 0; shift < 32; shift += 8)
                byte(static_cast<unsigned char>((bits >> shift) & 255U));
        }
        for (const auto& intent : learnedIntents()) text(intent.name);
        return hash;
    }();
    return fingerprint;
}

LearnedActionMask learnedIntentMask(const GameState& state) {
    if (state.self.race != Race::protoss) return 0;
    LearnedActionMask mask = 1; // waiting is always available
    const auto& intents = learnedIntents();
    for (std::size_t i = 1; i < intents.size(); ++i) {
        const auto& intent = intents[i];
        bool allowed = false;
        if (intent.technology != TechnologyKind::none) {
            const auto& stats = technologyStats(intent.technology);
            allowed = completed(state, stats.producer) &&
                !technologyInProgress(state.self, intent.technology) &&
                technologyLevel(state.self, intent.technology) < stats.maximumLevel;
            if (stats.maximumLevel > 1 && technologyLevel(state.self, intent.technology) >= 1) {
                const auto prerequisite = intent.technology == TechnologyKind::protossPlasmaShields ?
                    UnitKind::cyberneticsCore :
                    (intent.technology == TechnologyKind::protossAirWeapons ||
                     intent.technology == TechnologyKind::protossAirArmor) ?
                        UnitKind::fleetBeacon : UnitKind::templarArchives;
                allowed = allowed && completed(state, prerequisite);
            }
        } else {
            allowed = std::ranges::all_of(unitPrerequisites(intent.unit),
                [&state](auto prerequisite) { return completed(state, prerequisite); });
            if (isBuilding(intent.unit)) allowed = allowed && completed(state, UnitKind::probe);
            if (intent.unit == UnitKind::reaver)
                allowed = allowed && completed(state, UnitKind::roboticsSupportBay);
        }
        if (allowed) mask |= LearnedActionMask{1} << i;
    }
    return mask;
}

void ObservationEncoder::reset() noexcept {
    previousFrame_ = -1;
    previousMinerals_ = previousGas_ = 0;
    cached_.clear();
}

std::vector<float> ObservationEncoder::encode(const GameState& state) {
    if (state.frame < 0 || state.self.race != Race::protoss) { reset(); return {}; }
    if (state.frame == previousFrame_) return cached_;
    if (state.frame < previousFrame_) reset();
    std::vector<float> values;
    values.reserve(modelFeatures().size());
    const auto add = [&values](double value) { values.push_back(static_cast<float>(value)); };
    add(state.frame); add(state.self.minerals); add(state.self.gas);
    add(state.self.supplyUsed); add(state.self.supplyTotal);
    add(state.self.gatheredMinerals); add(state.self.gatheredGas);
    add(state.mapWidthPixels); add(state.mapHeightPixels);
    // Replay headers can reveal a Random opponent's resolved race. Only the
    // race established by a legal unit observation enters the v2 policy.
    auto observedRace = Race::unknown;
    for (const auto& unit : state.enemy.units) {
        if (unit.firstSeen >= 0 && unit.firstSeen <= state.frame &&
            (unit.race == Race::protoss || unit.race == Race::terran || unit.race == Race::zerg)) {
            observedRace = unit.race; break;
        }
    }
    add(observedRace == Race::protoss); add(observedRace == Race::terran);
    add(observedRace == Race::zerg); add(observedRace == Race::unknown);
    add(static_cast<double>(std::ranges::count_if(state.self.units, [](const auto& u) { return isWorker(u.kind) && u.gatheringGas; })));
    const bool historyReady = previousFrame_ >= 0 &&
        state.self.gatheredMinerals >= previousMinerals_ && state.self.gatheredGas >= previousGas_;
    const auto elapsed = historyReady ? state.frame - previousFrame_ : 0;
    add(historyReady); add(elapsed);
    add(elapsed > 0 ? 24.0 * (static_cast<double>(state.self.gatheredMinerals) - previousMinerals_) / elapsed : 0);
    add(elapsed > 0 ? 24.0 * (static_cast<double>(state.self.gatheredGas) - previousGas_) / elapsed : 0);

    struct Counts { int complete{}, incomplete{}, queued{}, visible{}, remembered{}; double age{}; };
    std::array<Counts, unitCount> counts{};
    for (const auto& unit : state.self.units) {
        const auto index = static_cast<std::size_t>(unit.kind);
        if (index >= unitCount || unit.hallucination) continue;
        if (unit.completed) ++counts[index].complete; else ++counts[index].incomplete;
    }
    for (const auto kind : state.self.queuedUnits) {
        const auto index = static_cast<std::size_t>(kind);
        if (index < unitCount) ++counts[index].queued;
    }
    for (const auto& unit : state.enemy.units) {
        const auto index = static_cast<std::size_t>(unit.kind);
        if (index >= unitCount || unit.lastSeen < 0 || unit.lastSeen > state.frame ||
            unit.firstSeen < 0 || unit.firstSeen > state.frame) continue;
        auto& count = counts[index];
        if (unit.visible) ++count.visible;
        else { ++count.remembered; count.age += std::min(240 * 24, state.frame - unit.lastSeen); }
    }
    for (std::size_t i = firstUnit; i < unitCount; ++i) {
        const auto& c = counts[i];
        add(c.complete); add(c.incomplete); add(c.queued); add(c.visible); add(c.remembered);
        add(c.remembered > 0 ? c.age / c.remembered : 0);
    }
    for (std::size_t i = 1; i < techCount; ++i) {
        const auto kind = static_cast<TechnologyKind>(i);
        add(technologyLevel(state.self, kind)); add(technologyInProgress(state.self, kind));
    }
    const auto& features = modelFeatures();
    if (values.size() != features.size()) throw std::logic_error("Observation schema mismatch");
    for (std::size_t i = 0; i < values.size(); ++i)
        values[i] = std::clamp(values[i] / features[i].scale, 0.0F, 16.0F);
    previousFrame_ = state.frame;
    previousMinerals_ = state.self.gatheredMinerals;
    previousGas_ = state.self.gatheredGas;
    cached_ = values;
    return values;
}
}  // namespace protodd
