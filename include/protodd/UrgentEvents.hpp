#pragma once

#include "GameState.hpp"

#include <cstdint>
#include <algorithm>
#include <span>

namespace protodd {

enum class UrgentEvent : std::uint8_t {
    cloakedThreat = 1U << 0U,
    workerLineBreach = 1U << 1U,
    builderLost = 1U << 2U,
    powerSourceLost = 1U << 3U,
    areaDamage = 1U << 4U,
};

[[nodiscard]] constexpr std::uint8_t urgentEventMask(const UrgentEvent event) noexcept {
    return static_cast<std::uint8_t>(event);
}

class UrgentEventQueue {
public:
    void enqueue(const UrgentEvent event) noexcept { pending_ |= urgentEventMask(event); }
    [[nodiscard]] std::uint8_t pending() const noexcept { return pending_; }
    [[nodiscard]] std::uint8_t consume() noexcept {
        const auto result = pending_;
        pending_ = 0;
        return result;
    }
    void clear() noexcept { pending_ = 0; }

private:
    std::uint8_t pending_{};
};

[[nodiscard]] inline bool hasNewUrgentUnitIds(
    const std::span<const UnitId> current,
    const std::span<const UnitId> previous) noexcept {
    // Both spans are kept sorted by the caller, so lookup stays logarithmic
    // instead of becoming quadratic as the visible army grows.
    return std::ranges::any_of(current, [previous](const UnitId id) {
        return !std::ranges::binary_search(previous, id);
    });
}

[[nodiscard]] inline bool isWorkerLineThreat(
    const UnitSnapshot& enemy,
    const std::span<const UnitSnapshot> friendlies,
    const int enemyWorkerRadiusPixels = 10 * 32,
    const int workerDepotRadiusPixels = 12 * 32) noexcept {
    if (!enemy.visible || !enemy.position.valid() || enemy.hallucination || enemy.loaded ||
        (enemy.groundWeapon.damage <= 0 && enemy.airWeapon.damage <= 0 &&
         !enemy.cloaked && !enemy.burrowed)) return false;
    const auto enemyRadiusSquared = enemyWorkerRadiusPixels * enemyWorkerRadiusPixels;
    const auto depotRadiusSquared = workerDepotRadiusPixels * workerDepotRadiusPixels;
    const auto maximumDepotRadius = enemyWorkerRadiusPixels + workerDepotRadiusPixels;
    const auto maximumDepotRadiusSquared = maximumDepotRadius * maximumDepotRadius;
    const auto nearFriendlyDepot = std::ranges::any_of(friendlies,
        [&enemy, maximumDepotRadiusSquared](const UnitSnapshot& unit) {
            return unit.role == UnitRole::resourceDepot && unit.position.valid() &&
                   distanceSquared(enemy.position, unit.position) <= maximumDepotRadiusSquared;
        });
    if (!nearFriendlyDepot) return false;
    for (const auto& worker : friendlies) {
        if (worker.role != UnitRole::worker || worker.loaded || !worker.position.valid() ||
            distanceSquared(enemy.position, worker.position) > enemyRadiusSquared) continue;
        const auto attachedToDepot = std::ranges::any_of(friendlies,
            [&worker, depotRadiusSquared](const UnitSnapshot& unit) {
                return unit.role == UnitRole::resourceDepot && unit.position.valid() &&
                       distanceSquared(worker.position, unit.position) <= depotRadiusSquared;
            });
        if (attachedToDepot) return true;
    }
    return false;
}

struct UrgentWork {
    bool updateMacro{};
    bool updateWorkers{};
    bool updateCombat{};
};

[[nodiscard]] constexpr UrgentWork urgentWorkFor(const std::uint8_t events) noexcept {
    constexpr auto cloaked = urgentEventMask(UrgentEvent::cloakedThreat);
    constexpr auto workerLine = urgentEventMask(UrgentEvent::workerLineBreach);
    constexpr auto builder = urgentEventMask(UrgentEvent::builderLost);
    constexpr auto power = urgentEventMask(UrgentEvent::powerSourceLost);
    constexpr auto area = urgentEventMask(UrgentEvent::areaDamage);
    return {
        .updateMacro = (events & (builder | power)) != 0,
        .updateWorkers = (events & (cloaked | workerLine | builder | area)) != 0,
        .updateCombat = (events & (cloaked | workerLine | area)) != 0,
    };
}

[[nodiscard]] constexpr UrgentWork frameWorkFor(
    const std::uint8_t events,
    const bool macroScheduled,
    const bool strategyUpdated,
    const bool workersScheduled,
    const bool combatScheduled) noexcept {
    const auto urgent = urgentWorkFor(events);
    return {
        .updateMacro = macroScheduled || strategyUpdated || urgent.updateMacro,
        .updateWorkers = workersScheduled || urgent.updateWorkers,
        .updateCombat = combatScheduled || urgent.updateCombat,
    };
}

[[nodiscard]] inline bool hasNewAreaDamageNearFriendlies(
    const std::span<const Position> current,
    const std::span<const Position> previous,
    const std::span<const UnitSnapshot> friendlies,
    const int mergeRadiusPixels = 48,
    const int dangerRadiusPixels = 160) noexcept {
    const auto mergeRadiusSquared = mergeRadiusPixels * mergeRadiusPixels;
    const auto dangerRadiusSquared = dangerRadiusPixels * dangerRadiusPixels;
    for (const auto center : current) {
        if (!center.valid()) continue;
        const auto alreadyActive = std::ranges::any_of(previous, [center, mergeRadiusSquared](
            const Position prior) {
            return prior.valid() && distanceSquared(center, prior) <= mergeRadiusSquared;
        });
        if (alreadyActive) continue;
        const auto threatensFriendly = std::ranges::any_of(friendlies,
            [center, dangerRadiusSquared](const UnitSnapshot& unit) {
                return !unit.loaded && unit.position.valid() &&
                       distanceSquared(center, unit.position) <= dangerRadiusSquared;
            });
        if (threatensFriendly) return true;
    }
    return false;
}

}  // namespace protodd
