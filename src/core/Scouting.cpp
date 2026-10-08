#include "protodd/Scouting.hpp"

#include "protodd/UnitCatalog.hpp"
#include "protodd/Combat.hpp"
#include "protodd/RouteSafety.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace protodd {
namespace {

struct Candidate {
    std::uint64_t key{};
    Position position;
    ScoutPurpose purpose;
    double value;
    Frame lastObserved{-1};
    Frame deadlineFrame{-1};
};

constexpr Frame scoutFrameSeconds(const int seconds) noexcept { return seconds * 24; }

std::uint64_t missionKey(const ScoutPurpose purpose, const Position target) noexcept {
    const auto x = static_cast<std::uint32_t>(std::max(0, target.x) / 128);
    const auto y = static_cast<std::uint32_t>(std::max(0, target.y) / 128);
    auto value = static_cast<std::uint64_t>(purpose) + 1;
    value = (value * 0x9e3779b185ebca87ULL) ^ (static_cast<std::uint64_t>(x) << 32U);
    return (value * 0xc2b2ae3d27d4eb4fULL) ^ y;
}

Frame revisitInterval(const ScoutPurpose purpose, const bool recentlyEmpty,
                      const ThreatAssessment& threat) noexcept {
    if (recentlyEmpty) {
        return scoutFrameSeconds(threat.expansion >= 0.60 ? 180 : 8 * 60);
    }
    switch (purpose) {
        case ScoutPurpose::findEnemy: return scoutFrameSeconds(120);
        case ScoutPurpose::checkTech: return scoutFrameSeconds(60);
        case ScoutPurpose::checkExpansion: return scoutFrameSeconds(120);
        case ScoutPurpose::watchArmy: return scoutFrameSeconds(24);
        case ScoutPurpose::patrolDropPath: return scoutFrameSeconds(24);
    }
    return scoutFrameSeconds(120);
}

double observerRouteRisk(const GameState& state, const UnitSnapshot& observer,
                         const InfluenceMap& influence,
                         Position from, Position to);

struct ScoutApproach {
    Position position{-1, -1};
    double risk{std::numeric_limits<double>::infinity()};
    RouteThreatProfile route{};
    Position routeWaypoint{-1, -1};
};

ScoutApproach chooseApproach(const GameState& state, const UnitSnapshot& scout,
                             const Candidate& candidate, const InfluenceMap& influence,
                             const NavigationGrid* terrain,
                             const Position previousWaypoint = {-1, -1}) {
    const auto observer = scout.kind == UnitKind::observer;
    const auto budget = observer ? 0.08 : scout.kind == UnitKind::probe ? 1.0 : 2.5;
    const auto sightRange = scout.sightRange > 0 ? scout.sightRange : 224;
    const auto ringRadius = std::clamp(sightRange - 32, 80, 256);
    const auto directAlternative = selectSafeRouteAlternative(influence, scout.position,
        candidate.position, scout.flying, terrain,
        {scout.dimensionLeft, scout.dimensionRight, scout.dimensionUp, scout.dimensionDown},
        budget, previousWaypoint);
    const auto directRoute = directAlternative.profile;
    const auto directRisk = observer
        ? observerRouteRisk(state, scout, influence, scout.position, candidate.position)
        : directRoute.score();
    const auto directSafe = directRoute.reachable && directRisk <= budget &&
        (!observer || ScoutManager::observerRouteSafe(
            state, scout, influence, candidate.position));
    const auto seekStandOff = directRisk > (observer ? 0.015 : 0.25);
    if (directSafe && !seekStandOff)
        return {candidate.position, directRisk, directRoute, directAlternative.waypoint};

    static constexpr Position directions[]{
        {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
        {1, 0}, {-1, 1}, {0, 1}, {1, 1},
    };
    ScoutApproach best;
    auto bestScore = std::numeric_limits<double>::infinity();
    for (const auto direction : directions) {
        const Position point{
            std::clamp(candidate.position.x + direction.x * ringRadius, 16,
                       std::max(16, state.mapWidthPixels - 17)),
            std::clamp(candidate.position.y + direction.y * ringRadius, 16,
                       std::max(16, state.mapHeightPixels - 17)),
        };
        if (point == candidate.position ||
            distance(point, candidate.position) > sightRange - 16) continue;
        const auto alternative = selectSafeRouteAlternative(
            influence, scout.position, point, scout.flying, terrain,
            {scout.dimensionLeft, scout.dimensionRight, scout.dimensionUp, scout.dimensionDown},
            budget, previousWaypoint);
        const auto route = alternative.profile;
        if (!route.reachable) continue;
        const auto risk = observer
            ? observerRouteRisk(state, scout, influence, scout.position, point)
            : route.score();
        if (risk > budget || (observer &&
            !ScoutManager::observerRouteSafe(state, scout, influence, point))) continue;
        const auto score = risk * (observer ? 12.0 : scout.kind == UnitKind::probe ? 8.0 : 2.0) +
            route.distance / 1000.0 + distance(point, candidate.position) / 2048.0;
        if (score < bestScore) {
            bestScore = score;
            best = {point, risk, route, alternative.waypoint};
        }
    }
    return best.position.valid() ? best : directSafe
        ? ScoutApproach{candidate.position, directRisk, directRoute} : ScoutApproach{};
}

bool informationPurpose(const ScoutPurpose purpose) noexcept {
    return purpose == ScoutPurpose::findEnemy || purpose == ScoutPurpose::checkTech ||
           purpose == ScoutPurpose::checkExpansion;
}

double observerRouteRisk(const GameState& state, const UnitSnapshot& observer,
                         const InfluenceMap& influence,
                         const Position from, const Position to) {
    const auto samples = std::max(1, static_cast<int>(std::ceil(
        distance(from, to) / 32.0)));
    auto average = 0.0;
    auto peak = 0.0;
    for (auto step = 0; step <= samples; ++step) {
        const auto ratio = static_cast<double>(step) / samples;
        const Position point{
            from.x + static_cast<int>(std::lround((to.x - from.x) * ratio)),
            from.y + static_cast<int>(std::lround((to.y - from.y) * ratio)),
        };
        const auto exposure = ScoutManager::observerExposure(
            state, observer, influence, point);
        average += exposure;
        peak = std::max(peak, exposure);
    }
    return average / static_cast<double>(samples + 1) + peak * 0.65;
}

Position friendlyMain(const GameState& state) {
    const auto depot = std::ranges::find_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot || unit.kind == UnitKind::nexus;
    });
    if (depot != state.self.units.end()) return depot->position;
    const auto base = std::ranges::find_if(state.bases, [&state](const BaseSnapshot& candidate) {
        return candidate.ownerId == state.self.id && candidate.center.valid();
    });
    return base != state.bases.end() ? base->center : Position{-1, -1};
}

double observerEscapeRisk(const GameState& state, const InfluenceMap& influence,
                          const UnitSnapshot& observer, const Position position) {
    auto risk = ScoutManager::observerExposure(state, observer, influence, position);
    const auto edge = std::min({position.x, position.y,
        state.mapWidthPixels - 1 - position.x,
        state.mapHeightPixels - 1 - position.y});
    if (edge < 160) risk += (160 - edge) / 160.0;
    return risk;
}

}  // namespace

std::string_view scoutPurposeName(const ScoutPurpose purpose) noexcept {
    switch (purpose) {
        case ScoutPurpose::findEnemy: return "find-enemy";
        case ScoutPurpose::checkTech: return "check-tech";
        case ScoutPurpose::checkExpansion: return "check-expansion";
        case ScoutPurpose::watchArmy: return "watch-army";
        case ScoutPurpose::patrolDropPath: return "patrol-drop-path";
    }
    return "unknown";
}

std::string_view scoutGapReasonName(const ScoutGapReason reason) noexcept {
    switch (reason) {
        case ScoutGapReason::none: return "none";
        case ScoutGapReason::noAvailableScout: return "no-available-scout";
        case ScoutGapReason::unsafeRoute: return "unsafe-route";
        case ScoutGapReason::lowerPriority: return "lower-priority";
    }
    return "unknown";
}

UnitId selectOpeningWorkerScout(
    const GameState& state,
    const std::span<const UnitId> previousScouts,
    const std::span<const UnitId> unavailableWorkers) noexcept {
    const auto enemyLocated = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.role == UnitRole::resourceDepot;
        });
    const auto enemyArmySeen = std::ranges::any_of(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.completed && isCombatUnit(unit.kind);
        });
    if (state.frame >= 6 * 60 * 24 || enemyLocated || enemyArmySeen ||
        std::ranges::none_of(state.self.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::pylon;
        })) {
        return -1;
    }

    const auto eligible = [&unavailableWorkers](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::probe && unit.completed &&
               !unit.loaded && !unit.disabled && !unit.hallucination &&
               !unit.carryingResources && !unit.underAttack &&
               std::ranges::find(unavailableWorkers, unit.id) ==
                   unavailableWorkers.end();
    };
    for (const auto previous : previousScouts) {
        const auto candidate = std::ranges::find(
            state.self.units, previous, &UnitSnapshot::id);
        if (candidate != state.self.units.end() && eligible(*candidate)) {
            return candidate->id;
        }
    }

    const UnitSnapshot* selected = nullptr;
    for (const auto& unit : state.self.units) {
        if (eligible(unit) && (selected == nullptr || unit.id < selected->id)) {
            selected = &unit;
        }
    }
    return selected != nullptr ? selected->id : -1;
}

void ProbeHarasser::reset() noexcept {
    withdrawing_ = finished_ = false;
    evadeUntil_ = 0;
    routeFrame_ = -1;
    returnWaypoint_ = {-1, -1};
}

std::optional<Command> ProbeHarasser::control(
    const GameState& state, const UnitId scoutId, const Position scoutGoal,
    const InfluenceMap& influence, const NavigationGrid* terrain) {
    if (finished_) return std::nullopt;
    const auto scout = state.findUnit(scoutId);
    if (!scout || !scout->ours || scout->kind != UnitKind::probe || !scout->completed || scout->loaded) {
        finished_ = true;
        return std::nullopt;
    }
    const auto home = friendlyMain(state);
    const auto armyUp = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
        return enemy.completed && !isWorker(enemy.kind) &&
            (isCombatUnit(enemy.kind) || enemy.groundWeapon.damage > 0);
    });
    // A scouted Core is enough warning to leave a worker line before the
    // first Dragoon arrives. Waiting to see the fighter can leave the Probe
    // behind it, with the only route home already covered by ranged fire.
    const auto rangedProduction = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& enemy) {
        return enemy.kind == UnitKind::cyberneticsCore && enemy.position.valid();
    });
    withdrawing_ = withdrawing_ || armyUp || rangedProduction || state.frame >= 6 * 60 * 24 ||
        scout->hitPoints < scout->maxHitPoints;
    const auto move = [&scout](const Position target, const char* reason, const int priority = 94) {
        return Command{scout->id, CommandType::move, -1, target,
                       UnitKind::unknown, priority, 0, reason};
    };
    if (withdrawing_) {
        if (!home.valid() || distanceSquared(scout->position, home) <= 160 * 160) {
            finished_ = true;
            return std::nullopt;
        }
        auto toward = home;
        auto routeUnavailable = false;
        if (terrain != nullptr && !terrain->empty()) {
            if (routeFrame_ < 0 || state.frame - routeFrame_ >= 24 ||
                distanceSquared(scout->position, returnWaypoint_) <= 48 * 48) {
                const auto route = terrain->nextWaypoint(scout->position, home);
                returnWaypoint_ = route.hasUsableWaypoint()
                    ? route.waypoint : Position{-1, -1};
                routeFrame_ = state.frame;
            }
            if (returnWaypoint_.valid()) toward = returnWaypoint_;
            else routeUnavailable = true;
        } else if (terrain != nullptr) {
            routeUnavailable = true;
        }
        const auto inDanger = std::ranges::any_of(state.enemy.units, [&scout](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && enemy.groundWeapon.damage > 0 &&
                weaponDistance(*scout, enemy) < enemy.groundWeapon.maxRange + 96;
        });
        if (routeUnavailable) {
            const auto step = influence.safestStep(scout->position, home, false);
            if (terrain != nullptr && !terrain->empty() &&
                !terrain->lineWalkable(scout->position, step,
                    MovementFootprint{scout->dimensionLeft, scout->dimensionRight,
                                      scout->dimensionUp, scout->dimensionDown})) {
                return Command{scout->id, CommandType::hold, -1, {-1, -1},
                    UnitKind::unknown, 100, 0, "probe-harass-route-unavailable"};
            }
            return move(step, inDanger ? "probe-harass-route-unavailable-evade"
                                       : "probe-harass-route-unavailable", 100);
        }
        auto step = inDanger ? influence.safestStep(scout->position, toward, false) : toward;
        if (terrain != nullptr && !terrain->empty() &&
            !terrain->lineWalkable(scout->position, step,
                MovementFootprint{scout->dimensionLeft, scout->dimensionRight,
                                  scout->dimensionUp, scout->dimensionDown})) {
            return Command{scout->id, CommandType::hold, -1, {-1, -1},
                UnitKind::unknown, 100, 0, "probe-harass-route-unavailable"};
        }
        return move(step, "probe-harass-withdraw", 100);
    }

    const UnitSnapshot* chaser = nullptr;
    const UnitSnapshot* target = nullptr;
    auto targetScore = std::numeric_limits<double>::infinity();
    auto closeWorkers = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!isWorker(enemy.kind) || !enemy.visible || !enemy.detected || !enemy.completed ||
            !enemy.position.valid() || enemy.invincible) continue;
        const auto range = weaponDistance(*scout, enemy);
        if (range < 80) ++closeWorkers;
        if (range < 224 && (enemy.orderTargetId == scoutId ||
            (scout->underAttack && range < 80))) {
            if (chaser == nullptr || range < weaponDistance(*scout, *chaser) ||
                (range == weaponDistance(*scout, *chaser) && enemy.id < chaser->id)) chaser = &enemy;
        }
        // Do not turn a worker passing our own mineral line into a chase.
        const auto atEnemyBase = std::ranges::any_of(state.enemy.units, [&enemy](const UnitSnapshot& depot) {
            return depot.role == UnitRole::resourceDepot && depot.position.valid() &&
                distanceSquared(depot.position, enemy.position) <= 512 * 512;
        });
        if (!atEnemyBase || range > 320 || !scout->canAttack(enemy)) continue;
        const auto score = range + enemy.durability() * 0.5;
        if (score < targetScore || (score == targetScore && (target == nullptr || enemy.id < target->id))) {
            targetScore = score;
            target = &enemy;
        }
    }
    const auto urgentEscape = chaser != nullptr || closeWorkers >= 2 || scout->underAttack ||
        (scout->maxShields > 0 && scout->shields < scout->maxShields / 2);
    if (urgentEscape) evadeUntil_ = state.frame + 24;
    if (scout->attackFrame && !urgentEscape) return std::nullopt;
    if (state.frame < evadeUntil_ || (target != nullptr && scout->weaponCooldown > state.latencyFrames + 2)) {
        const auto* danger = chaser != nullptr ? chaser : target;
        auto away = home.valid() ? home : scoutGoal;
        if (danger != nullptr) {
            away = {scout->position.x * 2 - danger->position.x,
                    scout->position.y * 2 - danger->position.y};
            if (away == scout->position) away = home;
        }
        auto escape = influence.safestStep(scout->position, away, false);
        // Check all visible workers before committing to an escape direction;
        // fleeing one defender must not run straight into a second defender.
        const auto clearance = [&state](const Position position) {
            auto closest = 1000.0;
            for (const auto& enemy : state.enemy.units)
                if (enemy.visible && isWorker(enemy.kind) && enemy.position.valid())
                    closest = std::min(closest, distance(position, enemy.position));
            return closest;
        };
        const auto homeStep = influence.safestStep(scout->position, home, false);
        if (home.valid() && clearance(homeStep) > clearance(escape) + 16.0) escape = homeStep;
        return move(escape, chaser != nullptr ? "probe-harass-evade-chaser" : "probe-harass-reset");
    }
    // Preserve the contact frame of our own attack unless escape is urgent.
    if (scout->attackFrame) return std::nullopt;
    if (target != nullptr) {
        if (weaponDistance(*scout, *target) <= 128)
            return Command{scoutId, CommandType::attackUnit, target->id, {-1, -1},
                UnitKind::unknown, 80, 0, "probe-harass-tag-worker"};
        return move(target->position, "probe-harass-approach", 70);
    }
    if (scoutGoal.valid()) return move(scoutGoal, "probe-scout-search", 60);
    return std::nullopt;
}

void ScoutManager::reset() noexcept {
    previousOrders_.clear();
    leaseGenerations_.clear();
    revisitAfter_.clear();
    retryAfter_.clear();
    missionMetrics_ = {};
    workerMissionStarted_ = -1;
    nextWorkerMission_ = 0;
    workerScout_ = -1;
    openingMission_ = false;
    returningMission_ = false;
    harasser_.reset();
    returnHarasser_.reset();
    observerEvadeUntil_.clear();
    observerEscapeWaypoint_.clear();
    informationGap_ = {};
}

void ScoutManager::forgetUnit(const UnitId id) noexcept {
    if (id < 0) return;
    if (const auto mission = previousOrders_.find(id); mission != previousOrders_.end()) {
        ++missionMetrics_.lost;
        const auto target = mission->second.order.informationTarget.valid()
            ? mission->second.order.informationTarget : mission->second.order.target;
        retryAfter_[missionKey(mission->second.order.purpose, target)] =
            mission->second.lastAccounted + scoutFrameSeconds(120);
    }
    previousOrders_.erase(id);
    leaseGenerations_.erase(id);
    observerEvadeUntil_.erase(id);
    observerEscapeWaypoint_.erase(id);
    if (workerScout_ != id) return;
    workerScout_ = -1;
    workerMissionStarted_ = -1;
    nextWorkerMission_ = 0;
    openingMission_ = false;
    returningMission_ = false;
    harasser_.reset();
    returnHarasser_.reset();
}

std::uint64_t ScoutManager::releaseLease(const UnitId id, const Frame frame) {
    if (id < 0) return 0;
    auto generation = std::uint64_t{};
    auto releasedMission = false;
    if (const auto mission = previousOrders_.find(id); mission != previousOrders_.end()) {
        generation = mission->second.order.leaseGeneration;
        const auto target = mission->second.order.informationTarget.valid()
            ? mission->second.order.informationTarget : mission->second.order.target;
        const auto key = missionKey(mission->second.order.purpose, target);
        retryAfter_[key] = std::max(retryAfter_[key], frame + scoutFrameSeconds(45));
        previousOrders_.erase(mission);
        ++missionMetrics_.cancelled;
        releasedMission = true;
    }
    if (workerScout_ == id) {
        if (!releasedMission && (openingMission_ || returningMission_))
            ++missionMetrics_.cancelled;
        workerScout_ = -1;
        workerMissionStarted_ = -1;
        nextWorkerMission_ = frame + scoutFrameSeconds(45);
        openingMission_ = false;
        returningMission_ = false;
        harasser_.reset();
        returnHarasser_.reset();
    }
    return generation;
}

void ScoutManager::recordCommandFeedback(
    const ScoutCommandFeedback& feedback, const Frame frame) {
    if (feedback.scout < 0 || feedback.status != ScoutCommandStatus::rejected) return;
    const auto mission = previousOrders_.find(feedback.scout);
    if (mission == previousOrders_.end() || feedback.leaseGeneration == 0 ||
        feedback.leaseGeneration != mission->second.order.leaseGeneration) return;
    const auto target = mission->second.order.informationTarget.valid()
        ? mission->second.order.informationTarget : mission->second.order.target;
    retryAfter_[missionKey(mission->second.order.purpose, target)] =
        frame + scoutFrameSeconds(30);
    ++missionMetrics_.rejected;
    ++missionMetrics_.cancelled;
    previousOrders_.erase(mission);
}

bool ScoutManager::observerInDanger(
    const GameState& state, const UnitSnapshot& observer,
    const InfluenceMap& influence) noexcept {
    if (observer.kind != UnitKind::observer || !observer.position.valid() ||
        !observer.completed || observer.loaded || observer.disabled) return false;
    if (observer.underAttack || observer.underStorm ||
        observer.hitPoints < observer.maxHitPoints / 2 ||
        observer.shields < observer.maxShields / 2) return true;
    return observerExposure(state, observer, influence, observer.position) > 0.08;
}

double ScoutManager::observerExposure(
    const GameState& state, const UnitSnapshot& observer,
    const InfluenceMap& influence, const Position position) noexcept {
    if (!position.valid()) return std::numeric_limits<double>::infinity();
    const auto storm = static_cast<double>(influence.stormDanger(position)) * 16.0;
    // Observers are permanently cloaked in Brood War, even when a fixture or
    // adapter omits the transient BWAPI isCloaked flag.
    const auto cloaked = observer.kind == UnitKind::observer || observer.cloaked;
    if (!cloaked) return storm + static_cast<double>(influence.at(position).airThreat) * 8.0;

    auto detected = false;
    auto airRisk = 0.0;
    const auto scanAvailable = state.enemy.race == Race::terran &&
        std::ranges::any_of(state.enemy.units, [&state](const UnitSnapshot& enemy) {
            if (enemy.kind != UnitKind::comsatStation || !enemy.completed ||
                enemy.disabled || enemy.energy < 50 || enemy.lastSeen > state.frame)
                return false;
            return state.frame - enemy.lastSeen <= 30 * 24;
        });
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.position.valid() || !enemy.completed || enemy.disabled || enemy.loaded ||
            enemy.hallucination || (unitStats(enemy.kind).requiresPsi && !enemy.powered) ||
            (!enemy.visible && !isBuilding(enemy.kind) &&
             (state.frame < enemy.lastSeen || state.frame - enemy.lastSeen > 8 * 24))) continue;
        const auto detector = enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::observer || enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::overlord || enemy.kind == UnitKind::photonCannon ||
            enemy.kind == UnitKind::missileTurret || enemy.kind == UnitKind::sporeColony;
        if (detector) {
            const auto radius = std::max(224, enemy.sightRange) + 128;
            detected = detected || distanceSquared(position, enemy.position) <= radius * radius;
        }
        if (enemy.airWeapon.damage <= 0 || !enemy.airWeapon.targetsAir) continue;
        const auto radius = enemy.airWeapon.maxRange + 96;
        const auto separation = distance(position, enemy.position);
        if (separation < radius)
            airRisk += std::max(0.10, 3.0 * (radius - separation) / std::max(1, radius));
    }
    constexpr auto scannerSweepRadius = 320;
    for (const auto sweep : state.scannerSweeps) {
        detected = detected || (sweep.valid() &&
            distanceSquared(position, sweep) <= scannerSweepRadius * scannerSweepRadius);
    }
    // A fresh, legally observed Comsat with enough recorded energy makes a
    // scan plausible anywhere on the map. Keep that as discounted risk;
    // active local detectors/scans remain full-strength evidence.
    return storm + (detected ? airRisk : scanAvailable ? airRisk * 0.35 : 0.0);
}

bool ScoutManager::observerRouteSafe(
    const GameState& state, const UnitSnapshot& observer,
    const InfluenceMap& influence, const Position target) noexcept {
    if (!observer.position.valid() || !target.valid()) return false;
    return observerRouteRisk(state, observer, influence,
                             observer.position, target) <= 0.08;
}

std::vector<Command> ScoutManager::protectObservers(
    const GameState& state, const InfluenceMap& influence, const std::span<const UnitId> escorts) {
    std::vector<Command> orders;
    const auto home = friendlyMain(state);
    for (const auto& observer : state.self.units) {
        if (observer.kind != UnitKind::observer || !observer.completed ||
            observer.loaded || observer.disabled || !observer.position.valid()) continue;
        const auto escort = std::ranges::find(escorts, observer.id) != escorts.end();
        if (escort) {
            previousOrders_.erase(observer.id);
            if (!observerInDanger(state, observer, influence)) {
                observerEvadeUntil_.erase(observer.id);
                observerEscapeWaypoint_.erase(observer.id);
                continue;
            }
        }
        const auto mission = previousOrders_.find(observer.id);
        const auto routeBlocked = mission != previousOrders_.end() &&
            !observerRouteSafe(state, observer, influence, mission->second.order.target);
        const auto urgent = observerInDanger(state, observer, influence) || routeBlocked;
        if (routeBlocked) {
            ++missionMetrics_.cancelled;
            const auto key = mission->second.order.informationTarget.valid()
                ? missionKey(mission->second.order.purpose,
                             mission->second.order.informationTarget)
                : missionKey(mission->second.order.purpose, mission->second.order.target);
            retryAfter_[key] = state.frame + scoutFrameSeconds(120);
            previousOrders_.erase(mission);
        }
        if (urgent) observerEvadeUntil_[observer.id] = state.frame + 5 * 24;
        const auto lease = observerEvadeUntil_.find(observer.id);
        if (lease != observerEvadeUntil_.end() && lease->second <= state.frame &&
            home.valid() && distanceSquared(observer.position, home) > 256 * 256)
            lease->second = state.frame + 24;
        if (lease == observerEvadeUntil_.end() || lease->second <= state.frame) continue;
        if (routeBlocked && home.valid() &&
            distanceSquared(observer.position, home) <= 256 * 256 &&
            !observerInDanger(state, observer, influence)) {
            observerEvadeUntil_.erase(lease);
            observerEscapeWaypoint_.erase(observer.id);
            orders.push_back({observer.id, CommandType::stop, -1, {-1, -1},
                              UnitKind::unknown, 110, 0, "observer-abort-unsafe-route"});
            continue;
        }
        if (!urgent && home.valid() &&
            distanceSquared(observer.position, home) <= 256 * 256) {
            observerEvadeUntil_.erase(lease);
            observerEscapeWaypoint_.erase(observer.id);
            continue;
        }
        // Score the whole nearby threat field. Fleeing just the nearest
        // detector can run into a second turret or Wraith, especially at an
        // edge where the old "away" point is outside the map.
        static constexpr Position directions[]{
            {-1, -1}, {0, -1}, {1, -1}, {-1, 0},
            {1, 0}, {-1, 1}, {0, 1}, {1, 1},
        };
        auto destination = observer.position;
        auto best = std::numeric_limits<double>::infinity();
        for (const auto direction : directions) {
            const Position candidate{
                std::clamp(observer.position.x + direction.x * 128, 16,
                           std::max(16, state.mapWidthPixels - 17)),
                std::clamp(observer.position.y + direction.y * 128, 16,
                           std::max(16, state.mapHeightPixels - 17)),
            };
            if (candidate == observer.position) continue;
            const auto halfway = moveToward(observer.position, candidate, 64.0);
            const auto danger = std::max(observerEscapeRisk(state, influence, observer, halfway),
                                         observerEscapeRisk(state, influence, observer, candidate));
            const auto homeCost = home.valid() ? distance(candidate, home) / 4096.0 : 0.0;
            const auto score = danger + homeCost;
            if (score + 0.001 < best) {
                best = score;
                destination = candidate;
            }
        }
        const auto previous = observerEscapeWaypoint_.find(observer.id);
        if (previous != observerEscapeWaypoint_.end() &&
            distanceSquared(observer.position, previous->second) > 40 * 40 &&
            distanceSquared(observer.position, previous->second) <= 192 * 192) {
            const auto halfway = moveToward(observer.position, previous->second, 64.0);
            const auto previousRisk = std::max(
                observerEscapeRisk(state, influence, observer, halfway),
                observerEscapeRisk(state, influence, observer, previous->second)) +
                (home.valid() ? distance(previous->second, home) / 4096.0 : 0.0);
            const auto improvesCurrent =
                observerEscapeRisk(state, influence, observer, previous->second) + 0.15 <
                observerEscapeRisk(state, influence, observer, observer.position);
            if (previousRisk <= best + 0.5 || improvesCurrent)
                destination = previous->second;
        }
        if (destination.valid()) observerEscapeWaypoint_[observer.id] = destination;
        if (destination.valid() && destination != observer.position)
            orders.push_back({observer.id, CommandType::move, -1, destination,
                              UnitKind::unknown, 110, 0, "observer-evade"});
    }
    std::erase_if(observerEvadeUntil_, [&state](const auto& entry) {
        return entry.second <= state.frame;
    });
    std::erase_if(observerEscapeWaypoint_, [this](const auto& entry) {
        return !observerEvadeUntil_.contains(entry.first);
    });
    return orders;
}

UnitId ScoutManager::selectWorkerScout(
    const GameState& state, const ThreatAssessment& threat,
    const std::span<const UnitId> previousScouts,
    const std::span<const UnitId> unavailableWorkers) {
    if (workerMissionStarted_ > state.frame) reset();
    if (openingMission_) {
        const auto worker = state.findUnit(workerScout_);
        if (worker && worker->ours && worker->completed &&
            std::ranges::find(unavailableWorkers, workerScout_) == unavailableWorkers.end())
            return workerScout_;
        openingMission_ = false;
        workerScout_ = -1;
        harasser_.finish();
        nextWorkerMission_ = state.frame + 45 * 24;
    }
    if (returningMission_) {
        const auto worker = state.findUnit(workerScout_);
        if (worker && worker->ours && worker->kind == UnitKind::probe &&
            worker->completed && std::ranges::find(unavailableWorkers, workerScout_) ==
                unavailableWorkers.end()) return workerScout_;
        returningMission_ = false;
        workerScout_ = -1;
        nextWorkerMission_ = state.frame + 45 * 24;
    }
    const auto opening = selectOpeningWorkerScout(state, previousScouts, unavailableWorkers);
    if (opening >= 0 && !harasser_.finished()) {
        workerScout_ = opening;
        workerMissionStarted_ = state.frame;
        openingMission_ = true;
        return opening;
    }
    const auto enoughWorkers = std::ranges::count_if(state.self.units, [](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::probe && unit.completed;
    }) >= 12;
    const auto safe = enoughWorkers && state.frame >= 3 * 60 * 24 &&
        state.frame < 8 * 60 * 24 && threat.combatEnemiesNearMain == 0 &&
        threat.immediateGround < 0.45 && threat.approachingArmyValue < 2.0 &&
        threat.workerRush <= 0.30 && threat.proxy + threat.staticContain <= 0.34;
    const auto eligible = [&unavailableWorkers](const UnitSnapshot& unit) {
        return unit.kind == UnitKind::probe && unit.completed && !unit.carryingResources &&
            !unit.underAttack && !unit.loaded &&
            std::ranges::find(unavailableWorkers, unit.id) == unavailableWorkers.end();
    };
    if (workerScout_ >= 0) {
        const auto worker = state.findUnit(workerScout_);
        if (safe && worker && eligible(*worker) &&
            state.frame - workerMissionStarted_ < 45 * 24) return workerScout_;
        // A timed-out scout can still be far across the map. Keep its lease
        // while it travels home; handing it to mining here sends it through
        // enemy territory with no scout escape control.
        const auto home = friendlyMain(state);
        if (worker && worker->ours && worker->kind == UnitKind::probe &&
            worker->completed && home.valid() &&
            distanceSquared(worker->position, home) > 160 * 160 &&
            std::ranges::find(unavailableWorkers, workerScout_) ==
                unavailableWorkers.end()) {
            returningMission_ = true;
            returnHarasser_.reset();
            returnHarasser_.withdraw();
            return workerScout_;
        }
        workerScout_ = -1;
        nextWorkerMission_ = state.frame + 45 * 24;
        return -1;
    }
    if (!safe || state.frame < nextWorkerMission_) return -1;
    const auto enemyKnown = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });
    if (!enemyKnown) return -1;
    for (const auto& candidate : state.self.units) {
        if (eligible(candidate) && (workerScout_ < 0 || candidate.id < workerScout_))
            workerScout_ = candidate.id;
    }
    if (workerScout_ >= 0) workerMissionStarted_ = state.frame;
    return workerScout_;
}

std::optional<Command> ScoutManager::controlWorkerScout(
    const GameState& state, const InfluenceMap& influence, const NavigationGrid* terrain) {
    if (returningMission_) {
        auto command = returnHarasser_.control(
            state, workerScout_, friendlyMain(state), influence, terrain);
        if (command) command->source = "probe-followup-return";
        if (returnHarasser_.finished()) {
            returningMission_ = false;
            workerScout_ = -1;
            nextWorkerMission_ = state.frame + 45 * 24;
        }
        return command;
    }
    if (!openingMission_) return std::nullopt;
    const auto previous = previousOrders_.find(workerScout_);
    const auto goal = previous != previousOrders_.end()
        ? previous->second.order.target : friendlyMain(state);
    const auto command = harasser_.control(state, workerScout_, goal, influence, terrain);
    if (harasser_.finished()) {
        openingMission_ = false;
        workerScout_ = -1;
        nextWorkerMission_ = state.frame + 45 * 24;
    }
    return command;
}

std::vector<ScoutOrder> ScoutManager::assign(
    const GameState& state,
    const std::span<const UnitId> availableScouts,
    const InfluenceMap& influence,
    const ThreatAssessment& threat,
    const NavigationGrid* terrain) {
    std::vector<Candidate> candidates;
    candidates.reserve(state.bases.size() + 4);
    informationGap_ = {};
    const auto natural = enemyNatural(state);
    const auto enemyDepot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });

    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) continue;
        auto purpose = ScoutPurpose::checkExpansion;
        if (base.startLocation && enemyDepot == state.enemy.units.end())
            purpose = ScoutPurpose::findEnemy;
        if (base.ownerId == state.enemy.id) purpose = ScoutPurpose::checkTech;

        const auto recentlyEmpty = base.ownerId != state.enemy.id &&
            base.lastConfirmedEmpty >= 0 && base.lastConfirmedEmpty >= base.lastScouted;
        const auto interval = revisitInterval(purpose, recentlyEmpty, threat);
        auto revisitFrame = base.lastScouted > 0
            ? base.lastScouted + interval : Frame{0};
        if (recentlyEmpty)
            revisitFrame = std::max(revisitFrame, base.lastConfirmedEmpty + interval);
        const auto key = missionKey(purpose, base.center);
        const auto rememberedVisit = revisitAfter_.find(key);
        if (rememberedVisit != revisitAfter_.end())
            revisitFrame = std::max(revisitFrame, rememberedVisit->second);
        const auto retry = retryAfter_.find(key);
        if (retry != retryAfter_.end() && retry->second > state.frame) continue;
        if (base.lastScouted > 0 && revisitFrame > state.frame) continue;
        if (recentlyEmpty && revisitFrame > state.frame) continue;

        const auto ageSeconds = std::max(0, state.frame - base.lastScouted) / 24.0;
        auto value = 2.0 + std::min(12.0, ageSeconds / 20.0);
        if (natural != nullptr && base.id == natural->id && ageSeconds >= 30.0)
            value += 12.0;
        if (purpose == ScoutPurpose::findEnemy) {
            value += state.frame < 5 * 60 * 24 ? 18.0 : 12.0;
        } else if (purpose == ScoutPurpose::checkTech) {
            value += 5.0 + threat.uncertainty * 5.0 +
                     std::max(threat.air, threat.cloak) * 4.0;
        } else if (base.ownerId == -1) {
            value += threat.expansion * 8.0;
        }
        auto deadlineFrame = base.lastScouted +
            (purpose == ScoutPurpose::findEnemy ? scoutFrameSeconds(5 * 60)
             : purpose == ScoutPurpose::checkTech ? scoutFrameSeconds(4 * 60)
                                                   : scoutFrameSeconds(8 * 60));
        if (recentlyEmpty)
            deadlineFrame = std::max(deadlineFrame, revisitFrame);
        candidates.push_back({key, base.center, purpose, value, base.lastScouted,
                              deadlineFrame});
    }
    if (enemyDepot != state.enemy.units.end() && enemyDepot->position.valid()) {
        const auto key = missionKey(ScoutPurpose::checkTech, enemyDepot->position);
        const auto age = std::max(0, state.frame - enemyDepot->lastSeen);
        const auto rememberedVisit = revisitAfter_.find(key);
        const auto retry = retryAfter_.find(key);
        const auto readyAt = std::max(enemyDepot->lastSeen + scoutFrameSeconds(60),
            rememberedVisit == revisitAfter_.end() ? Frame{0} : rememberedVisit->second);
        const auto alreadyThere = std::ranges::any_of(candidates, [enemyDepot](const Candidate& c) {
            return c.purpose == ScoutPurpose::checkTech &&
                distanceSquared(c.position, enemyDepot->position) <= 128 * 128;
        });
        if (!alreadyThere && age >= scoutFrameSeconds(60) && readyAt <= state.frame &&
            (retry == retryAfter_.end() || retry->second <= state.frame)) {
            candidates.push_back({key, enemyDepot->position, ScoutPurpose::checkTech,
                8.0 + threat.uncertainty * 7.0 + std::max(threat.air, threat.cloak) * 5.0,
                enemyDepot->lastSeen, readyAt});
        }
    }

    long long armyX = 0;
    long long armyY = 0;
    auto armyCount = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!isCombatUnit(enemy.kind) || !enemy.position.valid() ||
            state.frame - enemy.lastSeen > 20 * 24) continue;
        armyX += enemy.position.x;
        armyY += enemy.position.y;
        ++armyCount;
    }
    if (armyCount > 0) {
        const Position center{static_cast<int>(armyX / armyCount),
                              static_cast<int>(armyY / armyCount)};
        candidates.push_back({missionKey(ScoutPurpose::watchArmy, center), center,
            ScoutPurpose::watchArmy, 5.0 + threat.aggression * 9.0, -1, -1});
    }

    const auto enemyTransport = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::shuttle || unit.kind == UnitKind::dropship;
        });
    const auto ourMain = friendlyMain(state);
    if (enemyTransport != state.enemy.units.end() && ourMain.valid()) {
        const Position center{(enemyTransport->position.x + ourMain.x) / 2,
                              (enemyTransport->position.y + ourMain.y) / 2};
        candidates.push_back({missionKey(ScoutPurpose::patrolDropPath, center), center,
            ScoutPurpose::patrolDropPath, 10.0 + threat.air * 5.0, -1, -1});
    }

    std::unordered_set<UnitId> completedMissions;
    std::unordered_set<UnitId> lostMissions;
    std::unordered_set<UnitId> stalledMissions;
    for (auto& [scoutId, mission] : previousOrders_) {
        const auto scout = state.findUnit(scoutId);
        if (!scout) {
            ++missionMetrics_.lost;
            ++missionMetrics_.cancelled;
            const auto key = missionKey(mission.order.purpose,
                mission.order.informationTarget.valid()
                    ? mission.order.informationTarget : mission.order.target);
            retryAfter_[key] = state.frame + scoutFrameSeconds(120);
            lostMissions.insert(scoutId);
            continue;
        }
        const auto elapsed = std::max<Frame>(0, state.frame - mission.lastAccounted);
        if (scout->kind == UnitKind::probe)
            missionMetrics_.workerScoutFrames += static_cast<std::uint64_t>(elapsed);
        else
            missionMetrics_.detectorScoutFrames += static_cast<std::uint64_t>(elapsed);
        missionMetrics_.routeRiskFrames += mission.order.routeRisk * elapsed;
        mission.lastAccounted = state.frame;

        const auto infoTarget = mission.order.informationTarget.valid()
            ? mission.order.informationTarget : mission.order.target;
        const auto key = missionKey(mission.order.purpose, infoTarget);
        auto observedFrame = mission.observedAtStart;
        const auto base = std::ranges::find_if(state.bases, [infoTarget](const BaseSnapshot& item) {
            return distanceSquared(item.center, infoTarget) <= 128 * 128;
        });
        if (base != state.bases.end()) observedFrame = std::max(observedFrame, base->lastScouted);
        const auto observedDepot = std::ranges::find_if(
            state.enemy.units, [infoTarget](const UnitSnapshot& item) {
                return item.role == UnitRole::resourceDepot && item.position.valid() &&
                    distanceSquared(item.position, infoTarget) <= 128 * 128;
            });
        if (observedDepot != state.enemy.units.end())
            observedFrame = std::max(observedFrame, observedDepot->lastSeen);
        const auto sight = scout->sightRange > 0 ? scout->sightRange : 224;
        const auto distanceToTarget = static_cast<int>(distance(scout->position, infoTarget));
        const auto inObservationRange = distanceToTarget <= sight + 32;
        if (!mission.informationGained && observedFrame > mission.observedAtStart &&
            observedFrame <= state.frame && inObservationRange) {
            mission.informationGained = true;
            ++missionMetrics_.newInformation;
        }

        if (distanceToTarget + 32 < mission.bestDistance) {
            mission.bestDistance = distanceToTarget;
            mission.lastProgress = state.frame;
        }
        const auto reached = distanceToTarget <= std::max(64, sight - 24);
        const auto stalled = !reached && state.frame - mission.lastProgress >= scoutFrameSeconds(60);
        if (mission.informationGained || reached) {
            ++missionMetrics_.completed;
            const auto recentlyEmpty = base != state.bases.end() &&
                base->ownerId != state.enemy.id && base->lastConfirmedEmpty >= 0 &&
                base->lastConfirmedEmpty >= base->lastScouted;
            revisitAfter_[key] = state.frame + revisitInterval(
                mission.order.purpose, recentlyEmpty, threat);
            completedMissions.insert(scoutId);
        } else if (stalled) {
            ++missionMetrics_.cancelled;
            retryAfter_[key] = state.frame + scoutFrameSeconds(90);
            stalledMissions.insert(scoutId);
        }
    }
    for (const auto scoutId : lostMissions) previousOrders_.erase(scoutId);
    for (const auto scoutId : completedMissions) previousOrders_.erase(scoutId);
    for (const auto scoutId : stalledMissions) previousOrders_.erase(scoutId);

    std::unordered_set<std::uint64_t> claimed;
    std::vector<ScoutOrder> orders;
    std::unordered_map<UnitId, ScoutMission> nextOrders;
    // Gap reporting asks the same current-frame route question again. Reuse
    // only proofs without a previous waypoint, matching that query exactly.
    std::unordered_map<UnitId, std::vector<std::optional<ScoutApproach>>> approachesByScout;
    orders.reserve(availableScouts.size());
    for (const auto scoutId : availableScouts) {
        const auto scout = state.findUnit(scoutId);
        if (!scout) continue;
        if (scout->kind == UnitKind::observer &&
            (observerInDanger(state, *scout, influence) ||
             (observerEvadeUntil_.contains(scoutId) &&
              observerEvadeUntil_.at(scoutId) > state.frame))) continue;
        std::size_t bestIndex = 0;
        auto bestScore = -std::numeric_limits<double>::infinity();
        ScoutApproach bestApproach;
        const auto previous = previousOrders_.find(scoutId);
        struct RankedCandidate {
            std::size_t index{};
            double scoreUpperBound{};
            double continuationBonus{};
            Position previousWaypoint{-1, -1};
        };
        std::vector<RankedCandidate> ranked;
        ranked.reserve(candidates.size());
        const auto sightRange = scout->sightRange > 0 ? scout->sightRange : 224;
        const auto maximumStandOff = std::max(0, sightRange - 16);
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            const auto& candidate = candidates[i];
            if (claimed.contains(candidate.key) || (scout->kind == UnitKind::probe &&
                (candidate.purpose == ScoutPurpose::watchArmy ||
                 candidate.purpose == ScoutPurpose::patrolDropPath))) continue;
            const auto revisit = revisitAfter_.find(candidate.key);
            const auto retry = retryAfter_.find(candidate.key);
            if ((revisit != revisitAfter_.end() && revisit->second > state.frame) ||
                (retry != retryAfter_.end() && retry->second > state.frame)) continue;
            const auto sameMission = previous != previousOrders_.end() &&
                previous->second.order.purpose == candidate.purpose &&
                missionKey(candidate.purpose,
                    previous->second.order.informationTarget.valid()
                        ? previous->second.order.informationTarget
                        : previous->second.order.target) == candidate.key;
            auto previousWaypoint = sameMission
                ? previous->second.order.routeWaypoint : Position{-1, -1};
            if (previousWaypoint.valid() &&
                distanceSquared(scout->position, previousWaypoint) <= 96 * 96)
                previousWaypoint = {-1, -1};
            const auto bonus = sameMission &&
                distanceSquared(scout->position, previous->second.order.target) > 80 * 80 ? 4.0 : 0.0;
            // Every eligible stand-off lies within the scout's sight range of
            // the information target. Route distance cannot beat this chord
            // lower bound, and risk is nonnegative. No useful route is omitted
            // unless even its best possible score cannot win.
            const auto minimumTravel = std::max(0.0,
                distance(scout->position, candidate.position) - maximumStandOff) / 1000.0;
            ranked.push_back({i, candidate.value + bonus - minimumTravel, bonus, previousWaypoint});
        }
        std::ranges::sort(ranked, [](const RankedCandidate& left, const RankedCandidate& right) {
            return left.scoreUpperBound > right.scoreUpperBound ||
                (left.scoreUpperBound == right.scoreUpperBound && left.index < right.index);
        });
        auto& cachedApproaches = approachesByScout[scoutId];
        cachedApproaches.resize(candidates.size());
        for (const auto& rankedCandidate : ranked) {
            if (rankedCandidate.scoreUpperBound + 1e-9 < bestScore) break;
            const auto i = rankedCandidate.index;
            const auto& candidate = candidates[i];
            const auto approach = chooseApproach(
                state, *scout, candidate, influence, terrain, rankedCandidate.previousWaypoint);
            if (!rankedCandidate.previousWaypoint.valid()) cachedApproaches[i] = approach;
            if (!approach.position.valid()) continue;
            const auto travel = approach.route.distance / 1000.0;
            const auto riskWeight = scout->kind == UnitKind::probe
                ? 8.0 : scout->kind == UnitKind::observer ? 3.0 : 1.8;
            const auto score = candidate.value - approach.risk * riskWeight - travel +
                               rankedCandidate.continuationBonus;
            // Retain the original candidate order as the exact-score tie break
            // even though promising candidates are proved first.
            if (score > bestScore || (score == bestScore && i < bestIndex)) {
                bestScore = score;
                bestIndex = i;
                bestApproach = approach;
            }
        }
        if (std::isfinite(bestScore) && !candidates.empty() &&
            bestApproach.position.valid() && !claimed.contains(candidates[bestIndex].key)) {
            const auto& candidate = candidates[bestIndex];
            claimed.insert(candidate.key);
            const auto previous = previousOrders_.find(scoutId);
            const auto continuingLease = previous != previousOrders_.end() &&
                missionKey(previous->second.order.purpose,
                    previous->second.order.informationTarget.valid()
                        ? previous->second.order.informationTarget
                        : previous->second.order.target) == candidate.key;
            ScoutOrder order{scoutId, bestApproach.position, candidate.purpose, bestScore,
                             candidate.position, bestApproach.risk, candidate.deadlineFrame,
                             bestApproach.routeWaypoint};
            if (continuingLease) {
                order.leaseGeneration = previous->second.order.leaseGeneration;
            } else {
                auto& generation = leaseGenerations_[scoutId];
                if (generation < std::numeric_limits<std::uint64_t>::max()) ++generation;
                if (generation == 0) generation = 1;
                order.leaseGeneration = generation;
            }
            orders.push_back(order);
            if (continuingLease) {
                auto mission = previous->second;
                mission.order = order;
                mission.bestDistance = std::min(mission.bestDistance,
                    static_cast<int>(distance(scout->position, candidate.position)));
                mission.lastAccounted = state.frame;
                nextOrders.emplace(scoutId, std::move(mission));
            } else {
                nextOrders.emplace(scoutId, ScoutMission{
                    order, state.frame, state.frame, state.frame,
                    candidate.lastObserved,
                    static_cast<int>(distance(scout->position, candidate.position)), false});
            }
        }
    }

    for (const auto& [scoutId, mission] : previousOrders_) {
        if (nextOrders.contains(scoutId) || completedMissions.contains(scoutId) ||
            lostMissions.contains(scoutId) || stalledMissions.contains(scoutId)) continue;
        const auto target = mission.order.informationTarget.valid()
            ? mission.order.informationTarget : mission.order.target;
        const auto key = missionKey(mission.order.purpose, target);
        const auto handedOff = std::ranges::any_of(nextOrders, [key](const auto& entry) {
            const auto& next = entry.second.order;
            const auto nextTarget = next.informationTarget.valid()
                ? next.informationTarget : next.target;
            return missionKey(next.purpose, nextTarget) == key;
        });
        ++missionMetrics_.cancelled;
        if (!handedOff)
            retryAfter_[key] = std::max(retryAfter_[key], state.frame + scoutFrameSeconds(45));
    }

    const Candidate* nextInformation = nullptr;
    for (const auto& candidate : candidates) {
        if (!informationPurpose(candidate.purpose)) continue;
        if (nextInformation == nullptr || candidate.value > nextInformation->value)
            nextInformation = &candidate;
    }
    if (nextInformation != nullptr &&
        std::ranges::none_of(orders, [nextInformation](const ScoutOrder& order) {
            return informationPurpose(order.purpose) &&
                   order.purpose == nextInformation->purpose &&
                   distanceSquared(order.informationTarget, nextInformation->position) <= 128 * 128;
        })) {
        auto safestRouteRisk = std::numeric_limits<double>::infinity();
        const auto candidateIndex = static_cast<std::size_t>(nextInformation - candidates.data());
        for (const auto scoutId : availableScouts) {
            const auto scout = state.findUnit(scoutId);
            if (!scout) continue;
            const auto cached = approachesByScout.find(scoutId);
            const auto hasCached = cached != approachesByScout.end() &&
                candidateIndex < cached->second.size() && cached->second[candidateIndex].has_value();
            const auto approach = hasCached ? *cached->second[candidateIndex] : chooseApproach(
                state, *scout, *nextInformation, influence, terrain);
            safestRouteRisk = std::min(safestRouteRisk, approach.risk);
        }
        const auto observed = std::max<Frame>(0, nextInformation->lastObserved);
        const auto deadline = nextInformation->deadlineFrame >= 0
            ? nextInformation->deadlineFrame : state.frame + scoutFrameSeconds(4 * 60);
        const auto informationWindow = std::max<Frame>(24, deadline - observed);
        const auto ageRatio = std::clamp(
            static_cast<double>(std::max<Frame>(0, state.frame - observed)) /
                informationWindow, 0.0, 1.0);
        const auto overdueRatio = std::clamp(
            static_cast<double>(std::max<Frame>(0, state.frame - deadline)) /
                static_cast<double>(scoutFrameSeconds(2 * 60)), 0.0, 1.0);
        const auto routeRisk = std::isfinite(safestRouteRisk)
            ? std::clamp(safestRouteRisk / 4.0, 0.0, 1.0) : 0.5;
        informationGap_ = {
            true,
            nextInformation->position,
            nextInformation->purpose,
            deadline,
            std::clamp(0.20 + 0.35 * ageRatio + 0.25 * overdueRatio +
                           0.15 * std::clamp(threat.uncertainty, 0.0, 1.0) +
                           0.05 * routeRisk, 0.0, 1.0),
            availableScouts.empty() ? ScoutGapReason::noAvailableScout
                : (!std::isfinite(safestRouteRisk) || safestRouteRisk > 1.0
                    ? ScoutGapReason::unsafeRoute : ScoutGapReason::lowerPriority),
        };
    }
    previousOrders_ = std::move(nextOrders);
    std::erase_if(revisitAfter_, [&state](const auto& entry) {
        return entry.second + scoutFrameSeconds(10 * 60) < state.frame;
    });
    std::erase_if(retryAfter_, [&state](const auto& entry) {
        return entry.second <= state.frame;
    });
    return orders;
}
}  // namespace protodd
