#include "protodd/Scouting.hpp"

#include "protodd/UnitCatalog.hpp"
#include "protodd/Combat.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace protodd {
namespace {

struct Candidate {
    Position position;
    ScoutPurpose purpose;
    double value;
};

double routeRisk(
    const InfluenceMap& influence,
    const Position from,
    const Position to,
    const bool flying) {
    const auto samples = std::max(8, static_cast<int>(std::ceil(
        distance(from, to) / 64.0)));
    auto average = 0.0;
    auto peak = 0.0;
    for (auto step = 1; step <= samples; ++step) {
        const auto ratio = static_cast<double>(step) / static_cast<double>(samples);
        const Position point{
            from.x + static_cast<int>(std::lround((to.x - from.x) * ratio)),
            from.y + static_cast<int>(std::lround((to.y - from.y) * ratio)),
        };
        const auto cell = influence.at(point);
        const auto risk = static_cast<double>(flying ? cell.airThreat : cell.groundThreat);
        average += risk;
        peak = std::max(peak, risk);
    }
    return average / static_cast<double>(samples) + peak * 0.65;
}

bool unsafeObserverRoute(const InfluenceMap& influence,
                         const Position from, const Position to) {
    const auto samples = std::max(1, static_cast<int>(std::ceil(
        distance(from, to) / 64.0)));
    for (auto step = 0; step <= samples; ++step) {
        const auto ratio = static_cast<double>(step) / samples;
        const Position point{
            from.x + static_cast<int>(std::lround((to.x - from.x) * ratio)),
            from.y + static_cast<int>(std::lround((to.y - from.y) * ratio)),
        };
        const auto cell = influence.at(point);
        if (cell.airThreat > 0.08F || cell.detection > 0.08F ||
            influence.stormDanger(point) > 0.08F) return true;
    }
    return false;
}

bool terranObserverScreen(const GameState& state) {
    return state.enemy.race == Race::terran &&
        std::ranges::any_of(state.enemy.units, [&state](const UnitSnapshot& enemy) {
            return enemy.completed && enemy.position.valid() &&
                (enemy.visible || isBuilding(enemy.kind) ||
                 state.frame - enemy.lastSeen <= 30 * 24) &&
                (enemy.kind == UnitKind::missileTurret ||
                 enemy.kind == UnitKind::scienceVessel ||
                 enemy.kind == UnitKind::wraith ||
                 enemy.kind == UnitKind::goliath);
        });
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
                          const Position position) {
    const auto cell = influence.at(position);
    auto risk = static_cast<double>(cell.airThreat) * 8.0 +
                static_cast<double>(cell.detection) * 2.0 +
                static_cast<double>(influence.stormDanger(position)) * 12.0;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.position.valid() || !enemy.completed || enemy.disabled ||
            (!enemy.visible && !isBuilding(enemy.kind) &&
             state.frame - enemy.lastSeen > 8 * 24)) continue;
        const auto detector = enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::missileTurret;
        const auto weaponRadius = enemy.airWeapon.damage > 0
            ? enemy.airWeapon.maxRange + (enemy.kind == UnitKind::wraith ? 256 : 160) : 0;
        const auto detectorRadius = detector ? std::max(224, enemy.sightRange) + 128 : 0;
        const auto radius = std::max(weaponRadius, detectorRadius);
        if (radius == 0) continue;
        const auto separation = distance(position, enemy.position);
        if (separation < radius) {
            risk += (enemy.airWeapon.damage > 0 ? 3.0 : 1.25) *
                    (radius - separation) / static_cast<double>(radius);
        }
    }
    const auto edge = std::min({position.x, position.y,
        state.mapWidthPixels - 1 - position.x,
        state.mapHeightPixels - 1 - position.y});
    if (edge < 160) risk += (160 - edge) / 160.0;
    return risk;
}

}  // namespace

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
        if (terrain && !terrain->empty()) {
            if (routeFrame_ < 0 || state.frame - routeFrame_ >= 24 ||
                distanceSquared(scout->position, returnWaypoint_) <= 48 * 48) {
                returnWaypoint_ = terrain->nextWaypoint(scout->position, home);
                routeFrame_ = state.frame;
            }
            if (returnWaypoint_.valid()) toward = returnWaypoint_;
        }
        const auto inDanger = std::ranges::any_of(state.enemy.units, [&scout](const UnitSnapshot& enemy) {
            return enemy.visible && enemy.completed && enemy.groundWeapon.damage > 0 &&
                weaponDistance(*scout, enemy) < enemy.groundWeapon.maxRange + 96;
        });
        auto step = inDanger ? influence.safestStep(scout->position, toward, false) : toward;
        if (terrain && !terrain->empty() && !terrain->lineWalkable(scout->position, step)) step = toward;
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
    workerMissionStarted_ = -1;
    nextWorkerMission_ = 0;
    workerScout_ = -1;
    openingMission_ = false;
    returningMission_ = false;
    harasser_.reset();
    returnHarasser_.reset();
    observerEvadeUntil_.clear();
    observerEscapeWaypoint_.clear();
}

bool ScoutManager::observerInDanger(
    const GameState& state, const UnitSnapshot& observer,
    const InfluenceMap& influence) noexcept {
    if (observer.kind != UnitKind::observer || !observer.position.valid() ||
        !observer.completed || observer.loaded || observer.disabled) return false;
    if (observer.underAttack || observer.underStorm ||
        observer.hitPoints < observer.maxHitPoints / 2 ||
        observer.shields < observer.maxShields / 2) return true;
    const auto cell = influence.at(observer.position);
    if (cell.airThreat > 0.08F || cell.detection > 0.08F ||
        influence.stormDanger(observer.position) > 0.08F) return true;
    for (const auto& enemy : state.enemy.units) {
        if (!enemy.position.valid() || !enemy.completed || enemy.disabled ||
            (!enemy.visible && !isBuilding(enemy.kind) &&
             state.frame - enemy.lastSeen > 8 * 24)) continue;
        const auto detector = enemy.role == UnitRole::detector ||
            enemy.kind == UnitKind::scienceVessel ||
            enemy.kind == UnitKind::missileTurret;
        const auto reach = std::max(
            enemy.airWeapon.damage > 0
                ? enemy.airWeapon.maxRange + (enemy.kind == UnitKind::wraith ? 256 : 160) : 0,
            detector ? std::max(224, enemy.sightRange) + 128 : 0);
        if (reach > 0 && distanceSquared(observer.position, enemy.position) <=
                reach * reach) return true;
    }
    return false;
}

std::vector<Command> ScoutManager::protectObservers(
    const GameState& state, const InfluenceMap& influence) {
    std::vector<Command> orders;
    const auto home = friendlyMain(state);
    for (const auto& observer : state.self.units) {
        if (observer.kind != UnitKind::observer || !observer.completed ||
            observer.loaded || observer.disabled || !observer.position.valid()) continue;
        const auto mission = previousOrders_.find(observer.id);
        const auto routeBlocked = mission != previousOrders_.end() &&
            (unsafeObserverRoute(influence, observer.position, mission->second.target) ||
             (terranObserverScreen(state) &&
              (mission->second.purpose == ScoutPurpose::checkTech ||
               mission->second.purpose == ScoutPurpose::watchArmy)));
        const auto urgent = observerInDanger(state, observer, influence) || routeBlocked;
        if (routeBlocked) previousOrders_.erase(mission);
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
            const auto danger = std::max(observerEscapeRisk(state, influence, halfway),
                                         observerEscapeRisk(state, influence, candidate));
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
                observerEscapeRisk(state, influence, halfway),
                observerEscapeRisk(state, influence, previous->second)) +
                (home.valid() ? distance(previous->second, home) / 4096.0 : 0.0);
            const auto improvesCurrent =
                observerEscapeRisk(state, influence, previous->second) + 0.15 <
                observerEscapeRisk(state, influence, observer.position);
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
    const auto goal = previous != previousOrders_.end() ? previous->second.target : friendlyMain(state);
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
    const ThreatAssessment& threat) {
    std::vector<Candidate> candidates;
    candidates.reserve(state.bases.size() + 4);
    const auto natural = enemyNatural(state);
    const auto enemyDepot = std::ranges::find_if(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.role == UnitRole::resourceDepot;
    });

    for (const auto& base : state.bases) {
        if (!base.center.valid() || base.ownerId == state.self.id) {
            continue;
        }
        const auto staleSeconds = std::max(0, state.frame - base.lastScouted) / 24.0;
        auto value = std::min(12.0, 1.0 + staleSeconds / 15.0);
        auto purpose = ScoutPurpose::checkExpansion;
        if (natural != nullptr && base.id == natural->id && staleSeconds >= 30.0)
            value += 12.0;
        if (base.startLocation && enemyDepot == state.enemy.units.end()) {
            value += state.frame < 5 * 60 * 24 ? 18.0 : 12.0;
            purpose = ScoutPurpose::findEnemy;
        }
        if (base.ownerId == state.enemy.id) {
            value += 5.0 + threat.uncertainty * 5.0 +
                     std::max(threat.air, threat.cloak) * 4.0;
            purpose = ScoutPurpose::checkTech;
        } else if (base.ownerId == -1) {
            value += threat.expansion * 8.0;
        }
        candidates.push_back({base.center, purpose, value});
    }
    if (enemyDepot != state.enemy.units.end()) {
        candidates.push_back({enemyDepot->position, ScoutPurpose::checkTech,
                              8.0 + threat.uncertainty * 7.0 +
                                  std::max(threat.air, threat.cloak) * 5.0});
    }

    long long armyX = 0;
    long long armyY = 0;
    auto armyCount = 0;
    for (const auto& enemy : state.enemy.units) {
        if (!isCombatUnit(enemy.kind) || !enemy.position.valid() ||
            state.frame - enemy.lastSeen > 20 * 24) {
            continue;
        }
        armyX += enemy.position.x;
        armyY += enemy.position.y;
        ++armyCount;
    }
    if (armyCount > 0) {
        candidates.push_back({
            {static_cast<int>(armyX / armyCount), static_cast<int>(armyY / armyCount)},
            ScoutPurpose::watchArmy,
            5.0 + threat.aggression * 9.0,
        });
    }

    const auto enemyTransport = std::ranges::find_if(
        state.enemy.units, [](const UnitSnapshot& unit) {
            return unit.kind == UnitKind::shuttle || unit.kind == UnitKind::dropship;
        });
    const auto ourMain = friendlyMain(state);
    if (enemyTransport != state.enemy.units.end() && ourMain.valid()) {
        candidates.push_back({
            {(enemyTransport->position.x + ourMain.x) / 2,
             (enemyTransport->position.y + ourMain.y) / 2},
            ScoutPurpose::patrolDropPath,
            10.0 + threat.air * 5.0,
        });
    }

    std::unordered_set<std::size_t> claimed;
    std::vector<ScoutOrder> orders;
    std::unordered_map<UnitId, ScoutOrder> nextOrders;
    const auto terranAirScreen = terranObserverScreen(state);
    orders.reserve(availableScouts.size());
    for (const auto scoutId : availableScouts) {
        const auto scout = state.findUnit(scoutId);
        if (!scout) {
            continue;
        }
        if (scout->kind == UnitKind::observer &&
            (observerInDanger(state, *scout, influence) ||
             (observerEvadeUntil_.contains(scoutId) &&
              observerEvadeUntil_.at(scoutId) > state.frame))) continue;
        std::size_t bestIndex = 0;
        auto bestScore = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (scout->kind == UnitKind::observer && terranAirScreen &&
                (candidates[i].purpose == ScoutPurpose::checkTech ||
                 candidates[i].purpose == ScoutPurpose::watchArmy)) continue;
            if (claimed.contains(i) || (scout->kind == UnitKind::probe &&
                (candidates[i].purpose == ScoutPurpose::watchArmy ||
                 candidates[i].purpose == ScoutPurpose::patrolDropPath))) {
                continue;
            }
            const auto target = candidates[i].position;
            if (scout->kind == UnitKind::observer && unsafeObserverRoute(
                    influence, scout->position, target)) continue;
            const auto risk = routeRisk(influence, scout->position,
                                        target, scout->flying);
            // Refuse known dangerous worker missions; air scouts retain their
            // own risk-weighted policy.
            if (scout->kind == UnitKind::probe && risk > 1.0) continue;
            const auto travel = distance(scout->position, target) / 1000.0;
            const auto riskWeight = scout->kind == UnitKind::probe
                                        ? 8.0
                                        : (scout->kind == UnitKind::observer ? 3.0 : 1.8);
            auto score = candidates[i].value - risk * riskWeight - travel;
            const auto previous = previousOrders_.find(scoutId);
            if (previous != previousOrders_.end() &&
                previous->second.purpose == candidates[i].purpose &&
                distanceSquared(previous->second.target, target) < 128 * 128 &&
                distanceSquared(scout->position, target) > 112 * 112) {
                score += 3.0;
            }
            if (score > bestScore) {
                bestScore = score;
                bestIndex = i;
            }
        }
        if (std::isfinite(bestScore) && !candidates.empty() && !claimed.contains(bestIndex)) {
            claimed.insert(bestIndex);
            ScoutOrder order{scoutId, candidates[bestIndex].position,
                             candidates[bestIndex].purpose, bestScore};
            orders.push_back(order);
            nextOrders.emplace(scoutId, order);
        }
    }
    previousOrders_ = std::move(nextOrders);
    return orders;
}

}  // namespace protodd
