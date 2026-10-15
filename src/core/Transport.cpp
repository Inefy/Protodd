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

Frame unloadAcknowledgmentDeadline(const GameState& state,
                                   const UnitSnapshot& shuttle, const Position landing) {
    // Unload_All_Position first travels to the landing. Do not replace its
    // target on ordinary command latency while that native movement is live.
    const auto distance = std::hypot(static_cast<double>(shuttle.position.x - landing.x),
                                     static_cast<double>(shuttle.position.y - landing.y));
    const auto remainingTravel = std::max(0.0, distance - 32.0);
    const auto travelFrames = static_cast<int>(std::ceil(std::min(
        360.0, remainingTravel / std::max(0.5, shuttle.topSpeed))));
    const auto window = std::clamp(std::max(12, state.latencyFrames + 12) + 2 * travelFrames,
                                    12, 30 * 24);
    return state.frame + window;
}

Position unloadLanding(
    const GameState& state,
    const UnitSnapshot& reaver,
    const Position target,
    const int attempt,
    const NavigationGrid* navigation,
    const InfluenceMap* safeInfluence = nullptr) {
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
            if (safeInfluence != nullptr &&
                (safeInfluence->at(landing).groundThreat > 0.25F ||
                 safeInfluence->stormDanger(landing) > 0.0F || enemyNear(state, landing, 384)))
                continue;
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
        if (missions_.contains(shuttle->id) || cargoHandoffs_.contains(shuttle->id) ||
            state.frame < nextLaunch_[shuttle->id] ||
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
            if (assignedReavers.contains(reaver->id) || state.frame < nextLaunch_[reaver->id] ||
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
            static_cast<void>(advanceLeaseGeneration(shuttle->id));
            static_cast<void>(advanceLeaseGeneration(closest->id));
            missions_.insert_or_assign(shuttle->id, std::move(mission));
            assignedReavers.insert(closest->id);
        }
    }

    std::vector<Command> commands;
    std::vector<UnitId> finished;
    commands.reserve((missions_.size() + cargoHandoffs_.size()) * 2U);
    for (auto handoff = cargoHandoffs_.begin(); handoff != cargoHandoffs_.end();) {
        const auto shuttleId = handoff->first;
        auto& cargo = handoff->second;
        const auto* shuttle = findUnit(state, shuttleId);
        const auto* passenger = findUnit(state, cargo.passenger);
        if (shuttle == nullptr || passenger == nullptr ||
            (passenger->loaded && passenger->transportId != shuttleId)) {
            handoff = cargoHandoffs_.erase(handoff);
            continue;
        }
        const auto aboard = passenger->loaded && passenger->transportId == shuttleId;
        const auto acknowledgmentWindow = std::max(12, state.latencyFrames + 12);
        if (!aboard) {
            // A passenger's replacement order does not cancel the Shuttle's
            // native Load. Stop that separate actor through arbitration until
            // observation clears its passenger target, then watch latency.
            if (cargo.cancelPickup && shuttle->orderTargetId == cargo.passenger) {
                commands.push_back({shuttleId, CommandType::stop, -1, {-1, -1},
                    UnitKind::unknown, 112, 0, "transport-handoff-cancel-pickup"});
                cargo.releasedFrame = state.frame;
            }
            // A cancelled pickup can still cross command latency. Watch for
            // that effect without reserving an unloaded passenger from combat.
            if (cargo.cargoObserved ||
                state.frame - cargo.releasedFrame > acknowledgmentWindow) {
                handoff = cargoHandoffs_.erase(handoff);
                continue;
            }
            ++handoff;
            continue;
        }
        cargo.cargoObserved = true;
        if (retreat.valid()) cargo.retreat = retreat;
        // Let the confirmed emergency winner reach the native engine before
        // recovery may replace that same actor's order. Cargo stays owned.
        if (state.frame - cargo.releasedFrame <= acknowledgmentWindow) {
            ++handoff;
            continue;
        }
        // A Shuttle may spend the latency window braking an old flight.
        // Do not turn recovery into another same-actor retarget before the
        // accepted emergency Move makes world progress. A blocked actor still
        // reaches a bounded recovery fallback and keeps its loaded cargo.
        if (cargo.emergencyTarget.valid()) {
            const auto remaining = distance(shuttle->position, cargo.emergencyTarget);
            const auto initial = distance(cargo.emergencyStart, cargo.emergencyTarget);
            const auto observedProgress = remaining <= 32.0 || remaining + 16.0 <= initial;
            if (!observedProgress && state.frame < cargo.emergencyTravelDeadline) {
                ++handoff;
                continue;
            }
            cargo.emergencyTarget = {-1, -1};
            cargo.emergencyTravelDeadline = -1;
        }
        if (cargo.pendingUnload && state.frame >= cargo.unloadAcknowledgmentDeadline) {
            cargo.pendingUnload = false;
            cargo.unloadRequestFrame = -1;
            cargo.unloadAcknowledgmentDeadline = -1;
            ++cargo.failedUnloadAttempts;
        }
        if (!cargo.pendingUnload && cargo.retreat.valid()) {
            if (distanceSquared(shuttle->position, cargo.retreat) <= 192 * 192) {
                const auto landing = unloadLanding(state, *passenger, cargo.retreat,
                    cargo.failedUnloadAttempts, navigation, &influence);
                if (landing.valid() && influence.at(landing).groundThreat <= 0.25F &&
                    influence.stormDanger(landing) <= 0.0F && !enemyNear(state, landing, 384)) {
                    commands.push_back({shuttleId, CommandType::unload, -1, landing,
                        UnitKind::unknown, 112, 0, "transport-handoff-unload"});
                    cargo.pendingUnload = true;
                    cargo.unloadRequestFrame = state.frame;
                    cargo.unloadAcknowledgmentDeadline = unloadAcknowledgmentDeadline(state, *shuttle, landing);
                }
            } else {
                commands.push_back(moveCommand(shuttleId,
                    influence.safestStep(shuttle->position, cargo.retreat, true),
                    112, "transport-handoff-return"));
            }
        }
        ++handoff;
    }
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
            mission.unloadAcknowledgmentDeadline = -1;
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
            mission.unloadAcknowledgmentDeadline = -1;
                mission.pendingUnloadForReturn = false;
                mission.failedUnloadAttempts = 0;
            } else if (mission.unloadRequestFrame >= 0 &&
                       state.frame >= mission.unloadAcknowledgmentDeadline) {
                const auto failedAttackDrop = !mission.pendingUnloadForReturn;
                mission.pendingUnload = false;
                mission.unloadRequestFrame = -1;
            mission.unloadAcknowledgmentDeadline = -1;
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
                        mission.unloadAcknowledgmentDeadline = unloadAcknowledgmentDeadline(state, *shuttle, landing);
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
                        mission.unloadAcknowledgmentDeadline = unloadAcknowledgmentDeadline(state, *shuttle, landing);
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
    for (auto& command : commands) {
        command.owner = CommandOwner::transport;
        command.urgency = command.priority;
        command.deadlineFrame = state.frame;
        command.leaseGeneration = leaseGeneration(command.actor);
    }
    return commands;
}

std::uint64_t TransportController::advanceLeaseGeneration(const UnitId actor) {
    auto& generation = leaseGenerations_[actor];
    if (generation < std::numeric_limits<std::uint64_t>::max()) ++generation;
    return generation;
}

std::uint64_t TransportController::leaseGeneration(const UnitId actor) const noexcept {
    const auto found = leaseGenerations_.find(actor);
    return found == leaseGenerations_.end() ? 0 : found->second;
}

void TransportController::armEmergencyTravel(
    CargoHandoff& cargo, const GameState& state, const Command& winner, const UnitId shuttleId) {
    cargo.emergencyOwner = winner.owner;
    cargo.emergencyType = winner.type;
    cargo.emergencyUnit = winner.targetUnit;
    cargo.emergencyGeneration = winner.leaseGeneration;
    cargo.emergencyIntentPosition = winner.targetPosition;
    cargo.emergencyIntentValid = true;
    cargo.emergencyStart = {-1, -1};
    cargo.emergencyTarget = {-1, -1};
    cargo.emergencyTravelDeadline = -1;
    const auto* shuttle = findUnit(state, shuttleId);
    if (winner.actor != shuttleId || winner.type != CommandType::move ||
        !winner.targetPosition.valid() || shuttle == nullptr || !shuttle->position.valid()) return;
    cargo.emergencyStart = shuttle->position;
    cargo.emergencyTarget = winner.targetPosition;
    cargo.emergencyTravelDeadline = state.frame + std::max(96, 2 * state.latencyFrames + 24);
}

TransportHandoff TransportController::preemptActor(
    const GameState& state, const Command& winner, const bool confirmed,
    const Position retreat) {
    if (!confirmed || winner.owner == CommandOwner::transport ||
        !changesActorOrder(winner.type)) return {};
    const auto mission = std::ranges::find_if(missions_, [&winner](const auto& entry) {
        return entry.first == winner.actor || entry.second.reaver == winner.actor;
    });
    if (mission == missions_.end()) {
        // A new winner can cancel a proposed recovery unload. Clear that
        // acknowledgement timer while retaining the explicit cargo recovery.
        for (auto& [shuttleId, cargo] : cargoHandoffs_) {
            if (shuttleId != winner.actor) continue;
            const auto sameActiveIntent = winner.alreadyActive && cargo.emergencyIntentValid &&
                cargo.emergencyOwner == winner.owner && cargo.emergencyType == winner.type &&
                cargo.emergencyUnit == winner.targetUnit && cargo.emergencyGeneration == winner.leaseGeneration &&
                cargo.emergencyIntentPosition == winner.targetPosition;
            if (sameActiveIntent && !cargo.pendingUnload && !cargo.cancelPickup) return {};
            cargo.pendingUnload = false;
            cargo.unloadRequestFrame = -1;
            cargo.unloadAcknowledgmentDeadline = -1;
            cargo.cancelPickup = false;
            if (!sameActiveIntent) {
                cargo.releasedFrame = state.frame;
                armEmergencyTravel(cargo, state, winner, shuttleId);
            }
            return {shuttleId, cargo.passenger,
                advanceLeaseGeneration(shuttleId), leaseGeneration(cargo.passenger),
                cargo.cargoObserved};
        }
        return {};
    }
    const auto shuttleId = mission->first;
    const auto passengerId = mission->second.reaver;
    const auto* passenger = findUnit(state, passengerId);
    const auto aboard = passenger != nullptr && passenger->loaded &&
                        passenger->transportId == shuttleId;
    // Discard raid/load/shot intent; retain only cargo reconciliation. Both
    // actors stay unavailable to new raids through a bounded cooldown.
    missions_.erase(mission);
    nextLaunch_[shuttleId] = state.frame + 20 * 24;
    nextLaunch_[passengerId] = state.frame + 20 * 24;
    cargoHandoffs_.insert_or_assign(shuttleId,
        CargoHandoff{passengerId, state.frame, retreat, aboard, false, -1, 0,
                     winner.actor == passengerId});
    armEmergencyTravel(cargoHandoffs_.at(shuttleId), state, winner, shuttleId);
    return {shuttleId, passengerId, advanceLeaseGeneration(shuttleId),
            advanceLeaseGeneration(passengerId), aboard};
}

void TransportController::reset() {
    missions_.clear();
    cargoHandoffs_.clear();
    leaseGenerations_.clear();
    nextLaunch_.clear();
}

void TransportController::forgetUnit(const UnitId id) {
    if (id < 0) return;
    std::erase_if(missions_, [id](const auto& entry) {
        return entry.first == id || entry.second.reaver == id;
    });
    std::erase_if(cargoHandoffs_, [id](const auto& entry) {
        return entry.first == id || entry.second.passenger == id;
    });
    leaseGenerations_.erase(id);
    nextLaunch_.erase(id);
}

bool TransportController::ownsReaver(const UnitId id) const {
    return std::ranges::any_of(missions_, [id](const auto& entry) {
        return entry.second.reaver == id;
    }) || std::ranges::any_of(cargoHandoffs_, [id](const auto& entry) {
        return entry.second.passenger == id && entry.second.cargoObserved;
    });
}

}  // namespace protodd
