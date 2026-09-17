#include "protodd/Transport.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/Combat.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
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

Command moveCommand(
    const UnitId actor,
    const Position target,
    const int priority,
    const std::string_view source) {
    return {actor, CommandType::move, -1, target, UnitKind::unknown,
            priority, 0, std::string(source)};
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

    std::unordered_set<UnitId> armyReavers;
    for (auto i = 0; i < std::min(reservedArmyReavers, static_cast<int>(reavers.size())); ++i)
        armyReavers.insert(reavers[static_cast<std::size_t>(i)]->id);

    std::erase_if(missions_, [&state, &armyReavers, retreat](const auto& entry) {
        const auto* reaver = findUnit(state, entry.second.reaver);
        return findUnit(state, entry.first) == nullptr ||
               reaver == nullptr || (armyReavers.contains(reaver->id) && !reaver->loaded &&
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
        const UnitSnapshot* closest = nullptr;
        auto closestDistance = std::numeric_limits<int>::max();
        for (const auto* reaver : reavers) {
            if (assignedReavers.contains(reaver->id) ||
                (armyReavers.contains(reaver->id) && !reaver->loaded) ||
                (!reaver->loaded && (reaver->underAttack || reaver->healthFraction() < 0.65)) ||
                (reaver->loaded && reaver->transportId != shuttle->id)) {
                continue;
            }
            const auto candidate = distanceSquared(shuttle->position, reaver->position);
            if (candidate < closestDistance) {
                closestDistance = candidate;
                closest = reaver;
            }
        }
        if (closest != nullptr) {
            auto target = objective;
            auto waypoint = objective;
            if (economicTargets && !armyReavers.contains(closest->id)) {
                auto raider = *closest;
                raider.position = shuttle->position;
                raider.ammo = std::max(1, raider.ammo); // Target selection may precede loading Scarabs.
                const auto opportunity = harassmentOpportunity(state, raider, true, navigation);
                target = opportunity.target;
                waypoint = opportunity.waypoint;
                if (!target.valid()) continue;
            }
            missions_.insert_or_assign(
                shuttle->id, Mission{closest->id, TransportPhase::gathering, state.frame, target, waypoint});
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

        // The first splash units belong to the fighting army. If one was
        // already aboard when defense became urgent, bring it back and release
        // it after unloading instead of restarting the raid indefinitely.
        if (armyReavers.contains(reaver->id) && aboard)
            mission.phase = TransportPhase::returning;
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
            } else if (economicTargets && reaver->ammo < 2) {
                commands.push_back(moveCommand(shuttleId, reaver->position, 94, "shuttle-await-scarabs"));
                commands.push_back({reaver->id, CommandType::hold, -1, {-1, -1},
                                    UnitKind::unknown, 92, 0, "reaver-arm-for-drop"});
            } else if (separation <= 80 * 80) {
                commands.push_back({shuttleId, CommandType::load, reaver->id, {-1, -1},
                                    UnitKind::unknown, 97, 0, "reaver-load"});
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
                mission.phase = TransportPhase::returning;
                mission.transitionFrame = state.frame;
            } else if (distanceSquared(shuttle->position, mission.target) <=
                           (economicTargets ? 192 * 192 : 352 * 352) ||
                       (!economicTargets && enemyNear(state, shuttle->position, 288))) {
                if (localAirThreat <= 3.25F) {
                    const auto landing = navigation != nullptr && !navigation->empty()
                        ? navigation->nearestWalkable(mission.target, 3) : shuttle->position;
                    if (landing.valid() && (!economicTargets ||
                        distanceSquared(landing, mission.target) <= 192 * 192))
                        commands.push_back({shuttleId, CommandType::unload, -1,
                                            landing, UnitKind::unknown, 99, 0, "reaver-drop"});
                    else mission.phase = TransportPhase::returning;
                } else {
                    mission.phase = TransportPhase::returning;
                    mission.transitionFrame = state.frame;
                }
            } else {
                commands.push_back(moveCommand(
                    shuttleId, influence.safestStep(shuttle->position, mission.waypoint, true),
                    93, "shuttle-attack-route"));
            }
        }

        if (mission.phase == TransportPhase::extracting) {
            if (aboard) {
                mission.phase = TransportPhase::returning;
                mission.transitionFrame = state.frame;
                continue;
            }
            const auto exposedFor = state.frame - mission.transitionFrame;
            const auto dangerous = influence.at(reaver->position).groundThreat > 3.5F;
            const auto shouldExtract = exposedFor >= 7 * 24 ||
                                       reaver->healthFraction() < 0.58 || dangerous ||
                                       shuttle->healthFraction() < 0.45 ||
                                       armyReavers.contains(reaver->id) ||
                                       (economicTargets && reaver->ammo == 0) ||
                                       (economicTargets && !harassmentRouteSafe(state, *reaver, reaver->position)) ||
                                       !enemyNear(state, reaver->position, 448);
            if (shouldExtract) {
                if ((reaver->attackFrame || reaver->attackWindup) && !dangerous && reaver->healthFraction() >= 0.58) {
                    commands.push_back(moveCommand(shuttleId, reaver->position, 98, "shuttle-extract"));
                } else if (separation <= 80 * 80) {
                    commands.push_back({shuttleId, CommandType::load, reaver->id,
                                        {-1, -1}, UnitKind::unknown,
                                        99, 0, "reaver-extract"});
                } else {
                    commands.push_back(moveCommand(shuttleId, reaver->position, 98,
                                                   "shuttle-extract"));
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
                commands.push_back({shuttleId, CommandType::unload, -1, retreat,
                                    UnitKind::unknown, 96, 0, "reaver-return"});
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

bool TransportController::ownsReaver(const UnitId id) const {
    return std::ranges::any_of(missions_, [id](const auto& entry) { return entry.second.reaver == id; });
}

}  // namespace protodd
