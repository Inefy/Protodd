#pragma once

#include "protodd/Combat.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <ostream>
#include <span>
#include <string_view>

namespace protodd {

// Opt-in, fixed-capacity telemetry for checking local combat forecasts against
// later observed durability. It is intentionally separate from combat policy.
class CombatForecastAudit {
public:
    static constexpr std::size_t capacity = 32;
    static constexpr std::size_t unitCapacity = 64;
    static constexpr Frame simulationHorizonFrames = 24 * 14;

    enum class Censor : std::uint8_t {
        none, retreat, lostContact, rosterChanged, incomplete, matchEnd,
        friendlyEliminated, enemyEliminated, earlyResolution, horizonMismatch,
        horizonReached
    };

    struct Unit {
        UnitId id{-1};
        UnitKind kind{UnitKind::unknown};
        int initialDurability{};
        int latestDurability{};
        bool friendly{};
        bool healthKnown{};
        bool destroyed{};
    };

    struct Record {
        std::uint64_t id{};
        std::uint64_t engagementKey{};
        Frame startFrame{};
        Frame lastFrame{};
        Frame forecastFrame{-1};
        Frame actionFrame{-1};
        Frame outcomeFrame{-1};
        Frame closeFrame{-1};
        Frame lastTick{-1};
        std::uint16_t observations{};
        std::uint16_t unitCount{};
        double predictedFriendlyLoss{};
        double predictedFriendlySurvival{};
        int proposedAction{};
        int finalAction{};
        int forecastProposedAction{};
        int forecastFinalAction{};
        bool simulationAvailable{};
        bool outcomeObserved{};
        bool active{};
        bool emitted{};
        bool completeInitial{};
        bool resolved{};
        Censor censor{Censor::none};
        std::array<Unit, unitCapacity> units{};
        std::uint16_t forecastUnitCount{};
        bool forecastRosterComplete{};
        std::array<Unit, unitCapacity> forecastUnits{};
    };

    void setEnabled(bool enabled) noexcept { enabled_ = enabled; }
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    void reset(bool enabled = false) noexcept {
        enabled_ = enabled;
        matchSeed_ = mapHash_ = 0;
        gameId_ = 0;
        opponentId_ = 0;
        opponentRace_ = pvtStrategy_ = 0;
        nextId_ = created_ = flushed_ = censored_ = incomplete_ = 0;
        duplicates_ = droppedStartAttempts_ = writeErrors_ = 0;
        records_ = {};
    }
    [[nodiscard]] static constexpr std::uint64_t makeGameId(
        std::uint32_t seed, std::uint32_t mapHash, std::uint64_t opponentId,
        std::uint64_t startTicks) noexcept {
        auto hash = std::uint64_t{1469598103934665603ULL};
        for (const auto value : {static_cast<std::uint64_t>(seed),
                                 static_cast<std::uint64_t>(mapHash), opponentId, startTicks}) {
            for (unsigned shift = 0; shift < 64; shift += 8) {
                hash ^= (value >> shift) & 0xffU;
                hash *= 1099511628211ULL;
            }
        }
        return hash;
    }
    void setMatchContext(std::uint32_t seed, std::uint32_t mapHash, int opponentRace,
                         std::uint64_t gameId, std::uint64_t opponentId,
                         int pvtStrategy) noexcept {
        matchSeed_ = seed;
        mapHash_ = mapHash;
        opponentRace_ = opponentRace;
        gameId_ = gameId;
        opponentId_ = opponentId;
        pvtStrategy_ = pvtStrategy;
    }

    void beginTick(Frame) noexcept {}

    void observe(Frame frame, std::uint64_t engagementKey,
                 std::span<const UnitSnapshot> friendly,
                 std::span<const UnitSnapshot> enemy,
                 double predictedFriendlyLoss,
                 double predictedFriendlySurvival,
                 int proposedAction, int finalAction, bool simulationAvailable,
                 bool retreating) noexcept {
        if (!enabled_) return;
        auto* record = findActive(engagementKey);
        if (!record && enemy.empty()) return;
        if (!record) record = start(frame, engagementKey, friendly, enemy);
        if (!record || !record->active) return;
        if (record->lastTick == frame) { ++duplicates_; return; }
        record->lastTick = frame;
        record->lastFrame = frame;
        if (record->observations < std::numeric_limits<std::uint16_t>::max())
            ++record->observations;
        record->proposedAction = proposedAction;
        record->finalAction = finalAction;
        record->actionFrame = frame;
        if (!record->completeInitial) {
            close(*record, Censor::incomplete, false);
            return;
        }
        const auto rosterCensor = changedRoster(*record, friendly, enemy);
        if (rosterCensor != Censor::none) {
            close(*record, rosterCensor, false);
            return;
        }
        updateHealth(*record, friendly, true);
        updateHealth(*record, enemy, false);
        if (simulationAvailable && !record->simulationAvailable) {
            record->forecastFrame = frame;
            record->predictedFriendlyLoss = predictedFriendlyLoss;
            record->predictedFriendlySurvival = predictedFriendlySurvival;
            record->forecastProposedAction = proposedAction;
            record->forecastFinalAction = finalAction;
            captureForecastRoster(*record, friendly, enemy);
            record->simulationAvailable = true;
        }
        if (retreating) {
            close(*record, Censor::retreat, false);
            return;
        }
        if (record->simulationAvailable) {
            const auto elapsed = frame - record->forecastFrame;
            if (elapsed < simulationHorizonFrames &&
                (allDestroyed(*record, true) || allDestroyed(*record, false))) {
                close(*record, Censor::earlyResolution, false);
            } else if (elapsed >= simulationHorizonFrames) {
                record->outcomeObserved = true;
                close(*record, elapsed == simulationHorizonFrames
                    ? Censor::horizonReached : Censor::horizonMismatch, false);
            }
        } else if (allDestroyed(*record, true)) {
            close(*record, Censor::friendlyEliminated, true);
        } else if (allDestroyed(*record, false)) {
            close(*record, Censor::enemyEliminated, true);
        }
    }

    // Called only for a destruction BWAPI exposed as an observation. This is
    // not used to infer a hidden enemy's death.
    void observedDestroy(UnitId id) noexcept {
        if (!enabled_ || id < 0) return;
        for (auto& record : records_) {
            if (!record.active) continue;
            for (std::size_t i = 0; i < record.unitCount; ++i) {
                auto& unit = record.units[i];
                if (unit.id != id) continue;
                unit.destroyed = true;
                unit.latestDurability = 0;
            }
        }
    }

    void endTick(Frame frame) noexcept {
        if (!enabled_) return;
        for (auto& record : records_) {
            if (record.active && record.lastTick != frame) {
                record.closeFrame = frame;
                close(record, Censor::lostContact, false);
            }
        }
    }

    void finish(Frame frame) noexcept {
        if (!enabled_) return;
        for (auto& record : records_) {
            if (record.active) {
                record.closeFrame = frame;
                close(record, Censor::matchEnd, false);
            }
        }
    }

    void write(std::ostream& output) noexcept {
        if (!enabled_) return;
        for (auto& record : records_) {
            if (record.id == 0 || record.emitted) continue;
            try {
                output << "COMBAT_FORECAST," << record.id
                       << ",gameId=" << gameId_ << ",seed=" << matchSeed_ << ",mapHash=" << mapHash_
                       << ",opponentId=" << opponentId_ << ",opponentRace=" << opponentRace_
                       << ",pvtStrategy=" << pvtStrategy_
                       << ",key=" << record.engagementKey
                       << ",start=" << record.startFrame << ",end=" << record.lastFrame
                       << ",closeFrame=" << record.closeFrame
                       << ",forecastFrame=" << record.forecastFrame
                       << ",outcomeFrame=" << record.outcomeFrame
                       << ",expectedHorizonFrames=" << simulationHorizonFrames
                       << ",actionFrame=" << record.actionFrame
                       << ",observations=" << record.observations
                       << ",predictedLoss=" << record.predictedFriendlyLoss
                       << ",predictedSurvival=" << record.predictedFriendlySurvival
                       << ",proposedAction=" << record.proposedAction
                       << ",finalAction=" << record.finalAction
                       << ",forecastProposedAction=" << record.forecastProposedAction
                       << ",forecastFinalAction=" << record.forecastFinalAction
                       << ",simulation=" << record.simulationAvailable
                       << ",simulationRosterCoverage=unknown"
                       << ",outcomeObserved=" << record.outcomeObserved
                       << ",calibrationStatus=uncomparable_simulation_power_vs_hp_shields"
                       << ",resolved=" << record.resolved
                       << ",censor=" << censorName(record.censor)
                       << ",initialComplete=" << record.completeInitial
                       << ",friendlyLoss=" << observedLoss(record, true)
                       << ",enemyLoss=" << observedLoss(record, false)
                       << ",initial=";
                for (std::size_t i = 0; i < record.unitCount; ++i) {
                    const auto& unit = record.units[i];
                    if (i) output << ';';
                    output << (unit.friendly ? 'F' : 'E') << ':' << unit.id << ':'
                           << static_cast<int>(unit.kind) << ':' << unit.initialDurability
                           << ':' << unit.healthKnown;
                }
                output << ",latest=";
                for (std::size_t i = 0; i < record.unitCount; ++i) {
                    const auto& unit = record.units[i];
                    if (i) output << ';';
                    output << (unit.friendly ? 'F' : 'E') << ':' << unit.id << ':'
                           << unit.latestDurability << ':' << unit.healthKnown;
                }
                output << ",forecastRoster=";
                for (std::size_t i = 0; i < record.forecastUnitCount; ++i) {
                    const auto& unit = record.forecastUnits[i];
                    if (i) output << ';';
                    output << (unit.friendly ? 'F' : 'E') << ':' << unit.id << ':'
                           << static_cast<int>(unit.kind) << ':' << unit.initialDurability
                           << ':' << unit.healthKnown;
                }
                output << ",forecastRosterComplete=" << record.forecastRosterComplete
                       << ",outcomeRoster=";
                for (std::size_t i = 0; i < record.unitCount; ++i) {
                    const auto& unit = record.units[i];
                    if (i) output << ';';
                    output << (unit.friendly ? 'F' : 'E') << ':' << unit.id << ':'
                           << static_cast<int>(unit.kind) << ':' << unit.latestDurability
                           << ':' << unit.healthKnown;
                }
                output << '\n';
                if (output) ++flushed_;
                else ++writeErrors_;
            } catch (...) { ++writeErrors_; }
            record.emitted = true;
        }
        output << "COMBAT_FORECAST_SUMMARY,gameId=" << gameId_ << ",enabled=1,created=" << created_
               << ",flushed=" << flushed_ << ",censored=" << censored_
               << ",incomplete=" << incomplete_ << ",duplicates=" << duplicates_
               << ",droppedStartAttempts=" << droppedStartAttempts_
               << ",writeErrors=" << writeErrors_ << '\n';
    }

    [[nodiscard]] std::uint64_t created() const noexcept { return created_; }
    [[nodiscard]] std::uint64_t flushed() const noexcept { return flushed_; }
    [[nodiscard]] std::uint64_t censored() const noexcept { return censored_; }
    [[nodiscard]] std::uint64_t incomplete() const noexcept { return incomplete_; }
    [[nodiscard]] std::uint64_t duplicates() const noexcept { return duplicates_; }
    [[nodiscard]] std::uint64_t droppedStartAttempts() const noexcept { return droppedStartAttempts_; }
    [[nodiscard]] std::uint64_t writeErrors() const noexcept { return writeErrors_; }
    [[nodiscard]] const std::array<Record, capacity>& records() const noexcept { return records_; }

    [[nodiscard]] static constexpr std::string_view censorName(Censor censor) noexcept {
        switch (censor) {
        case Censor::none: return "open";
        case Censor::retreat: return "retreat";
        case Censor::lostContact: return "lost_contact";
        case Censor::rosterChanged: return "reinforcement_or_roster_change";
        case Censor::incomplete: return "incomplete_observation";
        case Censor::matchEnd: return "match_end";
        case Censor::friendlyEliminated: return "friendly_eliminated";
        case Censor::enemyEliminated: return "enemy_eliminated";
        case Censor::earlyResolution: return "early_resolution_before_horizon";
        case Censor::horizonMismatch: return "horizon_frame_mismatch";
        case Censor::horizonReached: return "horizon_reached";
        }
        return "unknown";
    }

private:
    [[nodiscard]] Record* findActive(std::uint64_t key) noexcept {
        for (auto& record : records_)
            if (record.active && record.engagementKey == key) return &record;
        return nullptr;
    }

    Record* start(Frame frame, std::uint64_t key,
                  std::span<const UnitSnapshot> friendly,
                  std::span<const UnitSnapshot> enemy) noexcept {
        Record* slot = nullptr;
        for (auto& record : records_) if (record.id == 0) { slot = &record; break; }
        if (!slot) { ++droppedStartAttempts_; return nullptr; }
        slot->id = ++nextId_;
        slot->engagementKey = key;
        slot->startFrame = slot->lastFrame = frame;
        slot->active = true;
        slot->completeInitial = true;
        if (friendly.empty() || enemy.empty()) slot->completeInitial = false;
        addInitial(*slot, friendly, true);
        addInitial(*slot, enemy, false);
        if (slot->unitCount == 0) slot->completeInitial = false;
        ++created_;
        return slot;
    }

    static void addInitial(Record& record, std::span<const UnitSnapshot> units,
                           bool friendly) noexcept {
        for (const auto& unit : units) {
            if (record.unitCount >= unitCapacity) { record.completeInitial = false; return; }
            const bool known = friendly || (unit.visible && unit.detected);
            auto& saved = record.units[record.unitCount++];
            saved.id = unit.id;
            saved.kind = unit.kind;
            saved.initialDurability = known ? std::max(0, unit.durability()) : 0;
            saved.latestDurability = saved.initialDurability;
            saved.friendly = friendly;
            saved.healthKnown = known && unit.id >= 0;
            record.completeInitial = record.completeInitial && saved.healthKnown;
        }
    }

    static void captureForecastRoster(Record& record,
                                     std::span<const UnitSnapshot> friendly,
                                     std::span<const UnitSnapshot> enemy) noexcept {
        record.forecastRosterComplete = true;
        const auto append = [&record](std::span<const UnitSnapshot> units, bool isFriendly) {
            for (const auto& unit : units) {
                if (record.forecastUnitCount >= unitCapacity) {
                    record.forecastRosterComplete = false;
                    return;
                }
                const bool known = isFriendly || (unit.visible && unit.detected);
                auto& saved = record.forecastUnits[record.forecastUnitCount++];
                saved.id = unit.id;
                saved.kind = unit.kind;
                saved.initialDurability = known ? std::max(0, unit.durability()) : 0;
                saved.latestDurability = saved.initialDurability;
                saved.friendly = isFriendly;
                saved.healthKnown = known && unit.id >= 0;
                record.forecastRosterComplete = record.forecastRosterComplete && saved.healthKnown;
            }
        };
        if (friendly.empty() || enemy.empty()) record.forecastRosterComplete = false;
        append(friendly, true);
        append(enemy, false);
    }

    static Censor changedRoster(const Record& record,
                                std::span<const UnitSnapshot> friendly,
                                std::span<const UnitSnapshot> enemy) noexcept {
        const auto contains = [](std::span<const UnitSnapshot> units, UnitId id) {
            return std::ranges::any_of(units, [id](const UnitSnapshot& unit) { return unit.id == id; });
        };
        for (const auto& unit : friendly) {
            bool known = false;
            for (std::size_t i = 0; i < record.unitCount; ++i)
                if (record.units[i].friendly && record.units[i].id == unit.id) { known = true; break; }
            if (!known) return Censor::rosterChanged;
        }
        for (const auto& unit : enemy) {
            if (!unit.visible || !unit.detected) return Censor::lostContact;
            bool known = false;
            for (std::size_t i = 0; i < record.unitCount; ++i)
                if (!record.units[i].friendly && record.units[i].id == unit.id) { known = true; break; }
            if (!known) return Censor::rosterChanged;
        }
        for (std::size_t i = 0; i < record.unitCount; ++i) {
            const auto& unit = record.units[i];
            if (unit.destroyed) continue;
            const auto present = unit.friendly ? contains(friendly, unit.id) : contains(enemy, unit.id);
            if (!present) return unit.friendly ? Censor::incomplete : Censor::lostContact;
        }
        return Censor::none;
    }

    static void updateHealth(Record& record, std::span<const UnitSnapshot> units,
                             bool friendly) noexcept {
        for (const auto& unit : units) {
            for (std::size_t i = 0; i < record.unitCount; ++i) {
                auto& saved = record.units[i];
                if (saved.id != unit.id || saved.friendly != friendly) continue;
                if (friendly || (unit.visible && unit.detected))
                    saved.latestDurability = std::max(0, unit.durability());
                break;
            }
        }
    }

    static bool allDestroyed(const Record& record, bool friendly) noexcept {
        bool found = false;
        for (std::size_t i = 0; i < record.unitCount; ++i) {
            const auto& unit = record.units[i];
            if (unit.friendly != friendly) continue;
            found = true;
            if (!unit.destroyed) return false;
        }
        return found;
    }

    void close(Record& record, Censor censor, bool resolved) noexcept {
        if (!record.active) return;
        record.active = false;
        record.resolved = resolved;
        record.censor = censor;
        record.outcomeFrame = record.lastFrame;
        if (record.closeFrame < 0) record.closeFrame = record.lastFrame;
        if (!resolved && censor != Censor::horizonReached) ++censored_;
        if (!record.completeInitial) ++incomplete_;
    }

    [[nodiscard]] static int observedLoss(const Record& record, bool friendly) noexcept {
        int result = 0;
        for (std::size_t i = 0; i < record.unitCount; ++i) {
            const auto& unit = record.units[i];
            if (unit.friendly == friendly && unit.healthKnown)
                result += std::max(0, unit.initialDurability - unit.latestDurability);
        }
        return result;
    }

    bool enabled_{};
    std::uint64_t gameId_{};
    std::uint32_t matchSeed_{};
    std::uint32_t mapHash_{};
    std::uint64_t opponentId_{};
    int opponentRace_{};
    int pvtStrategy_{};
    std::uint64_t nextId_{};
    std::uint64_t created_{};
    std::uint64_t flushed_{};
    std::uint64_t censored_{};
    std::uint64_t incomplete_{};
    std::uint64_t duplicates_{};
    std::uint64_t droppedStartAttempts_{};
    std::uint64_t writeErrors_{};
    std::array<Record, capacity> records_{};
};

}  // namespace protodd
