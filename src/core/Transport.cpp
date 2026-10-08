#include "protodd/Transport.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_set>

namespace protodd {
namespace {

const UnitSnapshot* findUnit(const GameState& state, const UnitId id) {
    const auto found = std::ranges::find(state.self.units, id, &UnitSnapshot::id);
    return found != state.self.units.end() ? &*found : nullptr;
}

bool enemyNear(const GameState& state, const Position position, const int radius) {
    return std::ranges::any_of(state.enemy.units, [position, radius](const UnitSnapshot& enemy) {
        return enemy.visible && !enemy.flying && enemy.position.valid() &&
               distanceSquared(enemy.position, position) <= radius * radius;
    });
}

bool hasSafeVolleyTarget(const GameState& state, const UnitSnapshot& reaver) {
    if (reaver.ammo <= 0 || !reaver.groundWeapon.targetsGround ||
        reaver.groundWeapon.maxRange <= 0 || !reaver.position.valid()) return false;
    const auto rangeSquared = reaver.groundWeapon.maxRange * reaver.groundWeapon.maxRange;
    return std::ranges::any_of(state.enemy.units, [&reaver, rangeSquared](const UnitSnapshot& enemy) {
        return enemy.visible && enemy.detected && !enemy.flying && !enemy.invincible &&
               enemy.position.valid() && reaver.canAttack(enemy) &&
               distanceSquared(reaver.position, enemy.position) <= rangeSquared;
    });
}

constexpr int reaverCargoSpace = 4;
constexpr int localDefenseRadius = 640;

bool threatensOwnedBase(const GameState& state, const BaseSnapshot& base) {
    return base.ownerId == state.self.id && base.center.valid() &&
        std::ranges::any_of(state.enemy.units, [&base](const UnitSnapshot& enemy) {
            return enemy.visible && !enemy.flying && enemy.position.valid() &&
                   (enemy.groundWeapon.targetsGround || isCombatUnit(enemy.kind)) &&
                   distanceSquared(enemy.position, base.center) <=
                       localDefenseRadius * localDefenseRadius;
        });
}

double reaverArmyUtility(
    const UnitSnapshot& reaver,
    const Position objective) {
    const auto objectiveDistance = objective.valid()
        ? std::sqrt(static_cast<double>(distanceSquared(reaver.position, objective)))
        : 0.0;
    const auto distanceCost = std::min(40.0, objectiveDistance / 48.0);
    return reaver.healthFraction() * 100.0 +
           static_cast<double>(std::clamp(reaver.ammo, 0, 5)) * 12.0 - distanceCost;
}

double reaverRaidUtility(
    const UnitSnapshot& reaver,
    const UnitSnapshot& shuttle,
    const std::vector<const BaseSnapshot*>& threatenedBases) {
    const auto rendezvousDistance = std::sqrt(static_cast<double>(
        distanceSquared(shuttle.position, reaver.position)));
    auto opportunityCost = 0.0;
    for (const auto* base : threatenedBases) {
        const auto baseDistance = std::sqrt(static_cast<double>(
            distanceSquared(reaver.position, base->center)));
        const auto proximity = std::clamp(
            (static_cast<double>(localDefenseRadius) - baseDistance) /
                static_cast<double>(localDefenseRadius),
            0.0, 1.0);
        opportunityCost += proximity * 48.0;
    }
    const auto distanceCost = std::min(60.0, rendezvousDistance / 36.0);
    return reaver.healthFraction() * 100.0 +
           static_cast<double>(std::clamp(reaver.ammo, 0, 5)) * 20.0 -
           distanceCost - opportunityCost;
}

Command moveCommand(
    const UnitId actor,
    const Position target,
    const int priority,
    const std::string_view source) {
    return {actor, CommandType::move, -1, target, UnitKind::unknown,
            priority, 0, std::string(source)};
}

Position unloadLanding(
    const GameState& state,
    const UnitSnapshot& reaver,
    const Position target,
    const int attempt,
    const NavigationGrid* navigation) {
    if (!target.valid()) return {-1, -1};
    constexpr std::array directions{
        Position{0, 0}, Position{0, -1}, Position{1, 0}, Position{0, 1},
        Position{-1, 0}, Position{1, -1}, Position{1, 1},
        Position{-1, 1}, Position{-1, -1},
    };
    const MovementFootprint footprint{
        reaver.dimensionLeft, reaver.dimensionRight,
        reaver.dimensionUp, reaver.dimensionDown};
    std::vector<Position> validLandings;
    for (const auto radius : {0, 64, 128, 192}) {
        for (const auto direction : directions) {
            if (radius == 0 && direction != directions.front()) continue;
            if (radius > 0 && direction == directions.front()) continue;
            const Position candidate{target.x + direction.x * radius,
                                     target.y + direction.y * radius};
            if (!candidate.valid() ||
                (state.mapWidthPixels > 0 && candidate.x >= state.mapWidthPixels) ||
                (state.mapHeightPixels > 0 && candidate.y >= state.mapHeightPixels)) continue;
            const auto landing = navigation != nullptr && !navigation->empty()
                ? navigation->nearestWalkable(candidate, 3, footprint) : candidate;
            if (!landing.valid() || distanceSquared(landing, target) > 192 * 192 ||
                std::ranges::find(validLandings, landing) != validLandings.end()) continue;
            if (navigation != nullptr && !navigation->empty()) {
                const auto reachable = navigation->nearestReachable(
                    landing, target, 3000, footprint);
                if (!reachable.hasUsableWaypoint() ||
                    distanceSquared(reachable.waypoint, target) > 192 * 192) continue;
            }
            validLandings.push_back(landing);
            if (validLandings.size() > static_cast<std::size_t>(std::max(0, attempt)))
                return validLandings.back();
        }
    }
    return {-1, -1};
}

}  // namespace

std::vector<Command> TransportController::control(
    const GameState& state,
    const Position objective,
    const Position retreat,
    const InfluenceMap& influence,
    const int reservedArmyReavers,
    const bool economicTargets, const NavigationGrid* navigation) {
    std::vector<const UnitSnapshot*> shuttles;
    std::vector<const UnitSnapshot*> reavers;
    for (const auto& unit : state.self.units) {
        if (!unit.completed || unit.disabled || unit.hallucination) continue;
        if (unit.kind == UnitKind::shuttle) shuttles.push_back(&unit);
        if (unit.kind == UnitKind::reaver) reavers.push_back(&unit);
    }
    std::ranges::sort(shuttles, {}, [](const UnitSnapshot* unit) { return unit->id; });
    std::ranges::sort(reavers, {}, [](const UnitSnapshot* unit) { return unit->id; });

    std::vector<const BaseSnapshot*> threatenedBases;
    for (const auto& base : state.bases) {
        if (threatensOwnedBase(state, base)) threatenedBases.push_back(&base);
    }
    std::ranges::sort(threatenedBases, {}, [](const BaseSnapshot* base) { return base->id; });

    std::unordered_set<UnitId> armyReavers;
    // Each threatened owned base gets its best nearby armed splash defender
    // before any general army reservation or transport assignment is made.
    for (const auto* base : threatenedBases) {
        const UnitSnapshot* defender = nullptr;
        auto bestDefenseUtility = -std::numeric_limits<double>::infinity();
        for (const auto* reaver : reavers) {
            if (reaver->ammo <= 0 || !reaver->position.valid() ||
                distanceSquared(reaver->position, base->center) >
                    localDefenseRadius * localDefenseRadius) continue;
            const auto distance = std::sqrt(static_cast<double>(
                distanceSquared(reaver->position, base->center)));
            const auto utility = reaver->healthFraction() * 100.0 +
                static_cast<double>(std::clamp(reaver->ammo, 0, 5)) * 12.0 - distance / 48.0;
            if (utility > bestDefenseUtility ||
                (utility == bestDefenseUtility && defender != nullptr && reaver->id < defender->id)) {
                defender = reaver;
                bestDefenseUtility = utility;
            }
        }
        if (defender != nullptr) armyReavers.insert(defender->id);
    }

    std::vector<const UnitSnapshot*> armyCandidates;
    for (const auto* reaver : reavers) {
        if (!armyReavers.contains(reaver->id)) armyCandidates.push_back(reaver);
    }
    std::ranges::sort(armyCandidates, [objective](
        const UnitSnapshot* left, const UnitSnapshot* right) {
        const auto leftUtility = reaverArmyUtility(*left, objective);
        const auto rightUtility = reaverArmyUtility(*right, objective);
        return leftUtility == rightUtility ? left->id < right->id : leftUtility > rightUtility;
    });
    const auto generalReservations = std::max(
        0, reservedArmyReavers - static_cast<int>(armyReavers.size()));
    for (auto i = 0; i < std::min(generalReservations,
                                  static_cast<int>(armyCandidates.size())); ++i) {
        armyReavers.insert(armyCandidates[static_cast<std::size_t>(i)]->id);
    }

    std::erase_if(missions_, [&state, &armyReavers, retreat](const auto& entry) {
        const auto* shuttle = findUnit(state, entry.first);
        const auto* reaver = findUnit(state, entry.second.reaver);
        return shuttle == nullptr || reaver == nullptr ||
               !shuttle->completed || shuttle->disabled || shuttle->hallucination ||
               !reaver->completed || reaver->disabled || reaver->hallucination ||
               (reaver->loaded && reaver->transportId != entry.first) ||
               (armyReavers.contains(reaver->id) && !reaver->loaded &&
                   distanceSquared(reaver->position, retreat) <= 256 * 256);
    });
    std::unordered_set<UnitId> assignedReavers;
    for (const auto& [shuttle, mission] : missions_) {
        static_cast<void>(shuttle);
        assignedReavers.insert(mission.reaver);
    }
    for (const auto* shuttle : shuttles) {
        if (missions_.contains(shuttle->id) || state.frame < nextLaunch_[shuttle->id] ||
            shuttle->healthFraction() < 0.65) continue;
        const auto alreadyHasReaver = std::ranges::any_of(
            reavers, [shuttle](const UnitSnapshot* reaver) {
                return reaver->loaded && reaver->transportId == shuttle->id;
            });
        if (!alreadyHasReaver && shuttle->cargoSpace < reaverCargoSpace) continue;
        const UnitSnapshot* closest = nullptr;
        auto closestTarget = objective;
        auto closestWaypoint = objective;
        auto bestUtility = -std::numeric_limits<double>::infinity();
        for (const auto* reaver : reavers) {
            if (alreadyHasReaver &&
                (!reaver->loaded || reaver->transportId != shuttle->id)) continue;
            if (assignedReavers.contains(reaver->id) ||
                (armyReavers.contains(reaver->id) && !reaver->loaded) ||
                (!reaver->loaded && (reaver->underAttack || reaver->healthFraction() < 0.65)) ||
                (reaver->loaded && reaver->transportId != shuttle->id)) {
                continue;
            }
            auto target = objective;
            auto waypoint = objective;
            if (economicTargets && !armyReavers.contains(reaver->id)) {
                auto raider = *reaver;
                raider.position = shuttle->position;
                raider.ammo = std::max(1, raider.ammo); // Target selection may precede loading Scarabs.
                const auto opportunity = harassmentOpportunity(state, raider, true, navigation);
                target = opportunity.target;
                waypoint = opportunity.waypoint;
                if (!target.valid()) continue;
            }
            const auto utility = reaverRaidUtility(*reaver, *shuttle, threatenedBases);
            if (utility > bestUtility ||
                (utility == bestUtility && closest != nullptr && reaver->id < closest->id)) {
                closest = reaver;
                closestTarget = target;
                closestWaypoint = waypoint;
                bestUtility = utility;
            }
        }
        if (closest != nullptr) {
            Mission mission{closest->id, TransportPhase::gathering, state.frame,
                            closestTarget, closestWaypoint};
            mission.observedAmmo = closest->ammo;
            missions_.insert_or_assign(shuttle->id, std::move(mission));
            assignedReavers.insert(closest->id);
        }
    }

    std::vector<Command> commands;
    std::vector<UnitId> finished;
    commands.reserve(missions_.size() * 2U);
    for (auto& [shuttleId, mission] : missions_) {
        const auto* shuttle = findUnit(state, shuttleId);
        const auto* reaver = findUnit(state, mission.reaver);
        if (shuttle == nullptr || reaver == nullptr) continue;
        const auto aboard = reaver->loaded && reaver->transportId == shuttleId;
        const auto separation = distanceSquared(shuttle->position, reaver->position);
        const auto acknowledgmentWindow = std::max(12, state.latencyFrames + 12);
        const auto enterReturning = [&mission, &state]() {
            mission.phase = TransportPhase::returning;
            mission.transitionFrame = state.frame;
            mission.loadRequestFrame = -1;
            mission.unloadRequestFrame = -1;
            mission.pendingUnload = false;
            mission.pendingUnloadForReturn = false;
        };
        if (mission.loadRequestFrame >= 0) {
            if (aboard) {
                mission.loadRequestFrame = -1;
                mission.failedLoadAttempts = 0;
            } else if (state.frame - mission.loadRequestFrame >= acknowledgmentWindow) {
                mission.loadRequestFrame = -1;
                ++mission.failedLoadAttempts;
            }
        }
        if (mission.pendingUnload) {
            if (!aboard) {
                mission.pendingUnload = false;
                mission.unloadRequestFrame = -1;
                mission.pendingUnloadForReturn = false;
                mission.failedUnloadAttempts = 0;
            } else if (mission.unloadRequestFrame >= 0 &&
                       state.frame - mission.unloadRequestFrame >= acknowledgmentWindow) {
                const auto failedAttackDrop = !mission.pendingUnloadForReturn;
                mission.pendingUnload = false;
                mission.unloadRequestFrame = -1;
                mission.pendingUnloadForReturn = false;
                ++mission.failedUnloadAttempts;
                if (failedAttackDrop && mission.failedUnloadAttempts >= 3)
                    enterReturning();
            }
        }
        const auto previousAmmo = mission.observedAmmo;
        const auto scarabLaunched = previousAmmo >= 0 && reaver->ammo < previousAmmo;
        const auto pendingShotLaunched = mission.waitingForScarabLaunch &&
            (scarabLaunched || reaver->ammo < mission.pendingShotAmmo || reaver->attackFrame);
        mission.observedAmmo = std::max(0, reaver->ammo);
        if (scarabLaunched) {
            mission.observedVolleys += std::max(1, previousAmmo - reaver->ammo);
            mission.scarabWaitExpired = false;
        }
        if (pendingShotLaunched) {
            mission.waitingForScarabLaunch = false;
            mission.pendingShotSince = -1;
            mission.scarabWaitExpired = false;
        }

        // The first splash units belong to the fighting army. If one was
        // already aboard when defense became urgent, bring it back and release
        // it after unloading instead of restarting the raid indefinitely.
        if (armyReavers.contains(reaver->id) && aboard &&
            mission.phase != TransportPhase::returning)
            enterReturning();
        else if (armyReavers.contains(reaver->id) && mission.phase != TransportPhase::returning)
            mission.phase = TransportPhase::extracting;

        if (mission.phase == TransportPhase::gathering) {
            if (!aboard && state.frame - mission.transitionFrame > 30 * 24) {
                finished.push_back(shuttleId);
                nextLaunch_[shuttleId] = state.frame + 20 * 24;
                continue;
            }
            if (aboard) {
                mission.phase = TransportPhase::attacking;
                mission.transitionFrame = state.frame;
                mission.loadRequestFrame = -1;
                mission.failedLoadAttempts = 0;
            } else if (mission.loadRequestFrame >= 0) {
                continue;
            } else if (economicTargets && reaver->ammo < 2) {
                commands.push_back(moveCommand(shuttleId, reaver->position, 94, "shuttle-await-scarabs"));
                commands.push_back({reaver->id, CommandType::hold, -1, {-1, -1},
                                    UnitKind::unknown, 92, 0, "reaver-arm-for-drop"});
            } else if (separation <= 80 * 80 && mission.loadRequestFrame < 0) {
                commands.push_back({shuttleId, CommandType::load, reaver->id, {-1, -1},
                                    UnitKind::unknown, 97, 0, "reaver-load"});
                mission.loadRequestFrame = state.frame;
            } else {
                commands.push_back(moveCommand(shuttleId, reaver->position, 94,
                                               "shuttle-rendezvous"));
                commands.push_back(moveCommand(reaver->id, shuttle->position, 92,
                                               "reaver-rendezvous"));
            }
        }

        if (mission.phase == TransportPhase::attacking) {
            if (!aboard) {
                mission.phase = TransportPhase::extracting;
                mission.transitionFrame = state.frame;
                mission.observedVolleys = 0;
                continue;
            }
            const auto localAirThreat = influence.at(shuttle->position).airThreat;
            if (distanceSquared(shuttle->position, mission.waypoint) <= 96 * 96)
                mission.waypoint = mission.target;
            auto targetStillSafe = true;
            if (economicTargets) {
                auto raider = *reaver;
                raider.position = shuttle->position;
                raider.ammo = std::max(1, raider.ammo);
                targetStillSafe = harassmentRouteSafe(state, raider, mission.waypoint, true);
                raider.position = mission.waypoint;
                targetStillSafe = targetStillSafe && harassmentRouteSafe(state, raider, mission.target, true);
            }
            if (!mission.target.valid() || !targetStillSafe || shuttle->healthFraction() < 0.42 ||
                localAirThreat > 4.5F || state.frame - mission.transitionFrame > 90 * 24) {
                enterReturning();
            } else if (distanceSquared(shuttle->position, mission.target) <=
                           (economicTargets ? 192 * 192 : 352 * 352) ||
                       (!economicTargets && enemyNear(state, shuttle->position, 288))) {
                if (mission.pendingUnload && aboard) {
                    continue;
                } else if (localAirThreat <= 3.25F) {
                    const auto landing = unloadLanding(
                        state, *reaver, mission.target, mission.failedUnloadAttempts, navigation);
                    if (landing.valid()) {
                        commands.push_back({shuttleId, CommandType::unload, -1,
                                            landing, UnitKind::unknown, 99, 0, "reaver-drop"});
                        mission.pendingUnload = true;
                        mission.pendingUnloadForReturn = false;
                        mission.unloadRequestFrame = state.frame;
                    } else {
                        enterReturning();
                    }
                } else {
                    enterReturning();
                }
            } else {
                commands.push_back(moveCommand(
                    shuttleId, influence.safestStep(shuttle->position, mission.waypoint, true),
                    93, "shuttle-attack-route"));
            }
        }

        if (mission.phase == TransportPhase::extracting) {
            if (aboard) {
                enterReturning();
                continue;
            }
            const auto exposedFor = state.frame - mission.transitionFrame;
            const auto dangerous = influence.at(reaver->position).groundThreat > 3.5F;
            constexpr Frame minimumVolleyExposure = 7 * 24;
            constexpr Frame maximumVolleyExposure = 11 * 24;
            const auto volleyPlanComplete = !hasSafeVolleyTarget(state, *reaver) ||
                reaver->ammo <= 0 || mission.observedVolleys >= 2 ||
                exposedFor >= maximumVolleyExposure;
            const auto plannedExtraction = exposedFor >= minimumVolleyExposure &&
                volleyPlanComplete;
            const auto shouldExtract = plannedExtraction ||
                                       reaver->healthFraction() < 0.58 || dangerous ||
                                       shuttle->healthFraction() < 0.45 ||
                                       armyReavers.contains(reaver->id) ||
                                       (economicTargets && reaver->ammo == 0) ||
                                       (economicTargets && !harassmentRouteSafe(state, *reaver, reaver->position)) ||
                                       !enemyNear(state, reaver->position, 448);
            const auto urgentPickup = dangerous || reaver->healthFraction() < 0.38 ||
                                      shuttle->healthFraction() < 0.30;
            if (mission.waitingForScarabLaunch) {
                const auto shotWaitLimit = std::max(12, state.latencyFrames + 12);
                if (!urgentPickup &&
                    state.frame - mission.pendingShotSince <= shotWaitLimit) {
                    const auto waitPoint = retreat.valid() ? retreat : reaver->position;
                    commands.push_back(moveCommand(
                        shuttleId, influence.safestStep(
                            shuttle->position, waitPoint, true), 98,
                        "shuttle-await-scarab-launch"));
                    continue;
                }
                mission.waitingForScarabLaunch = false;
                mission.pendingShotSince = -1;
                mission.scarabWaitExpired = true;
            }
            if (shouldExtract) {
                if (reaver->attackWindup && !reaver->attackFrame &&
                    reaver->ammo > 0 && !scarabLaunched && !urgentPickup &&
                    !mission.scarabWaitExpired) {
                    mission.waitingForScarabLaunch = true;
                    mission.pendingShotAmmo = reaver->ammo;
                    mission.pendingShotSince = state.frame;
                    const auto waitPoint = retreat.valid() ? retreat : reaver->position;
                    commands.push_back(moveCommand(
                        shuttleId, influence.safestStep(
                            shuttle->position, waitPoint, true), 98,
                        "shuttle-await-scarab-launch"));
                } else if ((reaver->attackFrame ||
                            (reaver->attackWindup && !mission.scarabWaitExpired)) &&
                           !urgentPickup && !dangerous && reaver->healthFraction() >= 0.58) {
                    commands.push_back(moveCommand(shuttleId,
                        influence.safestStep(shuttle->position, reaver->position, true),
                        98, "shuttle-extract"));
                } else if (separation <= 80 * 80 && mission.loadRequestFrame < 0) {
                    commands.push_back({shuttleId, CommandType::load, reaver->id,
                                        {-1, -1}, UnitKind::unknown,
                                        99, 0, "reaver-extract"});
                    mission.loadRequestFrame = state.frame;
                } else {
                    commands.push_back(moveCommand(shuttleId,
                        influence.safestStep(shuttle->position, reaver->position, true),
                        98, "shuttle-extract"));
                    commands.push_back(moveCommand(reaver->id, shuttle->position, 97,
                                                   "reaver-board"));
                }
            } else {
                CombatEstimate raid;
                raid.decision = FightDecision::engage;
                raid.ratio = 2.0;
                const auto firingOrders = TacticalController{}.control(
                    std::span<const UnitSnapshot>{reaver, 1}, state.enemy.units,
                    raid, mission.target, shuttle->position, influence, reaver->position,
                    state.latencyFrames, false, {},
                    economicTargets ? TacticalIntent::raid : TacticalIntent::battle);
                for (auto order : firingOrders) {
                    order.priority = 96;
                    if (order.type == CommandType::attackUnit) order.source = "reaver-economic-volley";
                    commands.push_back(std::move(order));
                }
                const auto screen = retreat.valid()
                                        ? moveToward(reaver->position, retreat, 112.0)
                                        : reaver->position;
                commands.push_back(moveCommand(
                    shuttleId, influence.safestStep(shuttle->position, screen, true),
                    88, "shuttle-cover"));
            }
        }

        if (mission.phase == TransportPhase::returning) {
            if (!aboard) {
                finished.push_back(shuttleId);
                nextLaunch_[shuttleId] = state.frame + 20 * 24;
                continue;
            }
            if (retreat.valid() &&
                distanceSquared(shuttle->position, retreat) <= 192 * 192) {
                if (!mission.pendingUnload) {
                    const auto landing = unloadLanding(
                        state, *reaver, retreat, mission.failedUnloadAttempts, navigation);
                    if (landing.valid()) {
                        commands.push_back({shuttleId, CommandType::unload, -1, landing,
                                            UnitKind::unknown, 96, 0, "reaver-return"});
                        mission.pendingUnload = true;
                        mission.pendingUnloadForReturn = true;
                        mission.unloadRequestFrame = state.frame;
                    } else {
                        commands.push_back(moveCommand(shuttleId,
                            influence.safestStep(shuttle->position, retreat, true),
                            96, "shuttle-retreat-route"));
                    }
                }
            } else if (retreat.valid()) {
                commands.push_back(moveCommand(
                    shuttleId, influence.safestStep(shuttle->position, retreat, true),
                    96, "shuttle-retreat-route"));
            }
        }
    }
    for (const auto shuttle : finished) missions_.erase(shuttle);
    std::ranges::sort(commands, {}, &Command::actor);
    return commands;
}

void TransportController::reset() {
    missions_.clear();
    nextLaunch_.clear();
}

void TransportController::forgetUnit(const UnitId id) {
    if (id < 0) return;
    std::erase_if(missions_, [id](const auto& entry) {
        return entry.first == id || entry.second.reaver == id;
    });
    nextLaunch_.erase(id);
}

bool TransportController::ownsReaver(const UnitId id) const {
    return std::ranges::any_of(missions_, [id](const auto& entry) { return entry.second.reaver == id; });
}

}  // namespace protodd
