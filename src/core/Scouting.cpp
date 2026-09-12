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
    constexpr auto samples = 8;
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
    harasser_.reset();
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
    orders.reserve(availableScouts.size());
    for (const auto scoutId : availableScouts) {
        const auto scout = state.findUnit(scoutId);
        if (!scout) {
            continue;
        }
        std::size_t bestIndex = 0;
        auto bestScore = -std::numeric_limits<double>::infinity();
        for (std::size_t i = 0; i < candidates.size(); ++i) {
            if (claimed.contains(i) || (scout->kind == UnitKind::probe &&
                (candidates[i].purpose == ScoutPurpose::watchArmy ||
                 candidates[i].purpose == ScoutPurpose::patrolDropPath))) {
                continue;
            }
            const auto risk = routeRisk(influence, scout->position,
                                        candidates[i].position, scout->flying);
            // Refuse known dangerous worker missions; air scouts retain their
            // own risk-weighted policy.
            if (scout->kind == UnitKind::probe && risk > 1.0) continue;
            const auto travel = distance(scout->position, candidates[i].position) / 1000.0;
            const auto riskWeight = scout->kind == UnitKind::probe
                                        ? 8.0
                                        : (scout->kind == UnitKind::observer ? 3.0 : 1.8);
            auto score = candidates[i].value - risk * riskWeight - travel;
            const auto previous = previousOrders_.find(scoutId);
            if (previous != previousOrders_.end() &&
                previous->second.purpose == candidates[i].purpose &&
                distanceSquared(previous->second.target, candidates[i].position) < 128 * 128 &&
                distanceSquared(scout->position, candidates[i].position) > 112 * 112) {
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
