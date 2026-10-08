#include "protodd/AllInOpening.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace protodd {
namespace {
int count(const GameState& state, const UnitKind kind, const bool completed = false) {
    const auto units = std::ranges::count_if(state.self.units, [=](const UnitSnapshot& unit) {
        return unit.kind == kind && !unit.hallucination && (!completed || unit.completed);
    });
    return static_cast<int>(units) + (completed ? 0 :
        static_cast<int>(std::ranges::count(state.self.queuedUnits, kind)));
}
void goal(StrategicPlan& plan, GoalKind kind, UnitKind target, int desired, int priority,
          bool blocking = true) {
    plan.goals.push_back({kind, target, desired, priority, blocking, "ladder opening commitment"});
}
struct Profile { int workers; int gates; int ready; Frame window; Frame travelTimeout; Frame deadline; };
void protectObservedDtBreach(StrategicPlan& plan, const GameState& state) {
    std::vector<std::pair<const UnitSnapshot*, const UnitSnapshot*>> baseThreats;
    for (const auto& enemy : state.enemy.units) {
        if (enemy.kind != UnitKind::darkTemplar || !enemy.visible || !enemy.completed ||
            enemy.hallucination || !enemy.position.valid()) continue;
        const UnitSnapshot* nearestBase = nullptr;
        auto nearestDistance = 1200 * 1200;
        for (const auto& own : state.self.units) {
            if (own.kind != UnitKind::nexus || own.id < 0 || !own.position.valid()) continue;
            const auto separation = distanceSquared(own.position, enemy.position);
            if (separation <= nearestDistance) {
                nearestDistance = separation;
                nearestBase = &own;
            }
        }
        if (nearestBase == nullptr) continue;
        const auto existing = std::ranges::find_if(baseThreats,
            [nearestBase](const auto& entry) { return entry.first->id == nearestBase->id; });
        if (existing == baseThreats.end()) baseThreats.emplace_back(nearestBase, &enemy);
        else if (distanceSquared(nearestBase->position, enemy.position) <
                 distanceSquared(existing->first->position, existing->second->position))
            existing->second = &enemy;
    }
    const auto framesTo = [](const Position from, const Position to, const double speed) {
        if (!from.valid() || !to.valid() || speed <= 0.0)
            return std::numeric_limits<Frame>::max();
        return static_cast<Frame>(std::ceil(distance(from, to) / speed));
    };
    for (const auto& [base, threat] : baseThreats) {
        if (threat->detected) continue;
        const auto threatSpeed = threat->topSpeed > 0.0 ? threat->topSpeed : 3.0;
        constexpr auto cannonDetectionRange = 224;
        const auto timeUntilBaseCoverage = static_cast<Frame>(std::floor(
            std::max(0.0, distance(base->position, threat->position) -
                              cannonDetectionRange) / threatSpeed));
        const auto coveredInTime = std::ranges::any_of(state.self.units,
            [&](const UnitSnapshot& detector) {
                if (!detector.position.valid() || detector.disabled || detector.loaded ||
                    detector.hallucination || detector.durability() <= 0)
                    return false;
                const auto sight = detector.sightRange > 0 ? detector.sightRange :
                    detector.kind == UnitKind::photonCannon ? cannonDetectionRange : 288;
                const auto coversThreat = distanceSquared(detector.position, threat->position) <=
                    sight * sight;
                if (detector.kind == UnitKind::photonCannon) {
                    if (!detector.powered) return false;
                    if (detector.completed && coversThreat) return true;
                    if (!detector.completed && detector.buildProgress >= 0 && coversThreat) {
                        const auto remaining = static_cast<Frame>(unitStats(detector.kind).buildTime) *
                            (100 - std::clamp(detector.buildProgress, 0, 100)) / 100;
                        if (remaining <= timeUntilBaseCoverage) return true;
                    }
                    return false;
                }
                if (detector.kind != UnitKind::observer || !detector.completed ||
                    detector.healthFraction() < 0.25) return false;
                if (coversThreat) return true;
                const auto speed = detector.topSpeed > 0.0 ? detector.topSpeed : 3.0;
                return framesTo(detector.position, threat->position, speed) <=
                       timeUntilBaseCoverage;
            });
        if (coveredInTime) continue;

        const auto siteRadius = 416 * 416;
        const auto localCannons = static_cast<int>(std::ranges::count_if(
            state.self.units, [base, siteRadius](const UnitSnapshot& own) {
                return own.kind == UnitKind::photonCannon && own.position.valid() &&
                       distanceSquared(own.position, base->position) <= siteRadius;
            }));
        const ConstructionTaskSite site{
            0x200000000ULL + static_cast<std::uint32_t>(base->id), -1, base->position};
        constexpr auto localPowerRadius = 128 * 128;
        const auto hasLocalPylon = std::ranges::any_of(state.self.units,
            [base](const UnitSnapshot& own) {
                return own.kind == UnitKind::pylon && own.durability() > 0 && own.position.valid() &&
                       distanceSquared(own.position, base->position) <= localPowerRadius;
            });
        if (!hasLocalPylon) {
            const auto localPylons = static_cast<int>(std::ranges::count_if(
                state.self.units, [base](const UnitSnapshot& own) {
                    return own.kind == UnitKind::pylon && own.durability() > 0 && own.position.valid() &&
                           distanceSquared(own.position, base->position) <= localPowerRadius;
                }));
            plan.goals.push_back({GoalKind::build, UnitKind::pylon, localPylons + 1,
                131, true, "power local DT detection at the threatened base",
                TechnologyKind::none, false, false, site});
        }
        plan.goals.push_back({GoalKind::build, UnitKind::photonCannon, localCannons + 1,
            130, true, "local powered detection for observed DT breach",
            TechnologyKind::none, false, false, site});
    }
}
Position recoveryExpansion(const GameState& state) {
    const BaseSnapshot* best = nullptr;
    for (const auto& base : state.bases) {
        if (base.ownerId >= 0 || base.island || !base.depotFootprintAvailable ||
            base.startLocation || base.mineralsRemaining <= 0 ||
            base.groundDistanceFromMain <= 0 || !base.center.valid()) continue;
        const bool occupied = std::ranges::any_of(state.enemy.units, [&base](const UnitSnapshot& unit) {
            return unit.visible && unit.position.valid() &&
                   distanceSquared(unit.position, base.center) <= 640 * 640;
        });
        if (occupied) continue;
        if (!best || base.groundDistanceFromMain < best->groundDistanceFromMain) best = &base;
    }
    return best ? best->center : Position{-1, -1};
}
Profile profile(const AllInBuild build) {
    switch (build) {
        case AllInBuild::twoGateZealot: return {14, 2, 4, 120 * 24, 90 * 24, 9000};
        case AllInBuild::threeGateDragoon: return {20, 3, 4, 120 * 24, 90 * 24, 12000};
        case AllInBuild::fourGateDragoon: return {20, 4, 6, 150 * 24, 90 * 24, 13200};
        case AllInBuild::darkTemplar: return {18, 2, 1, 90 * 24, 60 * 24, 10800};
        case AllInBuild::standard: break;
    }
    return {};
}
}

AllInBuild allInBuild(const std::string_view name) noexcept {
    if (name.starts_with("two-gate-zealot")) return AllInBuild::twoGateZealot;
    if (name.starts_with("three-gate-dragoon")) return AllInBuild::threeGateDragoon;
    if (name.starts_with("four-gate-dragoon")) return AllInBuild::fourGateDragoon;
    if (name.starts_with("dt-pressure")) return AllInBuild::darkTemplar;
    return AllInBuild::standard;
}
std::string_view allInBuildName(const AllInBuild build) noexcept {
    switch (build) {
        case AllInBuild::twoGateZealot: return "two-gate-zealot";
        case AllInBuild::threeGateDragoon: return "three-gate-dragoon";
        case AllInBuild::fourGateDragoon: return "four-gate-dragoon";
        case AllInBuild::darkTemplar: return "dt-pressure";
        case AllInBuild::standard: return "standard";
    }
    return "standard";
}
std::string_view allInPhaseName(const AllInPhase phase) noexcept {
    switch (phase) {
        case AllInPhase::assemble: return "assemble";
        case AllInPhase::pressure: return "pressure";
        case AllInPhase::transition: return "transition";
    }
    return "invalid";
}
void AllInOpeningPlanner::reset(const AllInBuild build) noexcept {
    build_ = build; phase_ = AllInPhase::assemble; launch_ = departure_ = arrival_ = contact_ =
        pressureStart_ = -1; peakArmy_ = 0; reason_ = "none";
}
void AllInOpeningPlanner::apply(StrategicPlan& plan, const GameState& state,
                               const ThreatAssessment& threat) {
    if (!active()) return;
    const auto p = profile(build_);
    const bool zealot = build_ == AllInBuild::twoGateZealot;
    const bool dt = build_ == AllInBuild::darkTemplar;
    const auto mainUnit = zealot ? UnitKind::zealot : dt ? UnitKind::darkTemplar : UnitKind::dragoon;
    const auto ready = count(state, mainUnit, true);
    const auto army = count(state, UnitKind::zealot, true) + count(state, UnitKind::dragoon, true) +
                      count(state, UnitKind::darkTemplar, true);
    peakArmy_ = std::max(peakArmy_, army);
    // Economic failure never locks us into rebuilding the same one-base push.
    if (phase_ != AllInPhase::transition) {
        if (state.frame >= p.deadline) { phase_ = AllInPhase::transition; reason_ = "deadline"; }
        else if (ready >= p.ready && launch_ < 0) {
            phase_ = AllInPhase::pressure; launch_ = state.frame;
        }
        if (phase_ == AllInPhase::pressure && launch_ >= 0) {
            const auto home = std::ranges::find(state.self.units, UnitKind::nexus,
                                                &UnitSnapshot::kind);
            const auto homePosition = home != state.self.units.end() ? home->position : Position{-1, -1};
            const auto pressureArmy = [&](const UnitSnapshot& unit) {
                return unit.completed && !unit.hallucination &&
                    (unit.kind == UnitKind::zealot || unit.kind == UnitKind::dragoon ||
                     unit.kind == UnitKind::darkTemplar);
            };
            const auto away = std::ranges::count_if(state.self.units, [&](const UnitSnapshot& unit) {
                return pressureArmy(unit) && homePosition.valid() && unit.position.valid() &&
                       distanceSquared(homePosition, unit.position) > 800 * 800;
            });
            if (departure_ < 0 && away >= std::max(1, p.ready / 2)) departure_ = state.frame;
            if (arrival_ < 0 && plan.attackTarget.valid()) {
                const auto atObjective = std::ranges::count_if(state.self.units,
                    [&](const UnitSnapshot& unit) {
                        return pressureArmy(unit) && unit.position.valid() &&
                            distanceSquared(plan.attackTarget, unit.position) <= 640 * 640;
                    });
                if (atObjective >= std::max(1, p.ready / 2)) arrival_ = state.frame;
            }
            if (contact_ < 0) {
                const auto madeContact = std::ranges::any_of(state.self.units,
                    [&](const UnitSnapshot& own) {
                        if (!pressureArmy(own) || !own.position.valid()) return false;
                        return std::ranges::any_of(state.enemy.units, [&](const UnitSnapshot& enemy) {
                            if (!enemy.visible || !enemy.completed || !enemy.position.valid() ||
                                enemy.hallucination) return false;
                            const auto weapon = enemy.flying ? own.airWeapon : own.groundWeapon;
                            return own.canAttack(enemy) &&
                                distanceSquared(own.position, enemy.position) <=
                                    (weapon.maxRange + 32) * (weapon.maxRange + 32);
                        });
                    });
                if (madeContact) contact_ = state.frame;
            }
            // Arrival is a fallback clock; first weapon-range contact earns
            // the full bounded engagement window when it happens later.
            if (contact_ >= 0) pressureStart_ = contact_;
            else if (pressureStart_ < 0 && arrival_ >= 0) pressureStart_ = arrival_;
            if (pressureStart_ >= 0 && state.frame - pressureStart_ >= p.window) {
                phase_ = AllInPhase::transition; reason_ = "pressure-window";
            } else if (arrival_ < 0 &&
                       state.frame - (departure_ >= 0 ? departure_ : launch_) >= p.travelTimeout) {
                phase_ = AllInPhase::transition; reason_ = "travel-timeout";
            } else if (state.frame - launch_ >= 24 * 12 && peakArmy_ >= p.ready &&
                       army <= std::max(1, peakArmy_ / 3)) {
                phase_ = AllInPhase::transition; reason_ = "army-loss";
            }
        }
    }
    plan.name += " [" + std::string(allInBuildName(build_)) + ":" +
                 std::string(allInPhaseName(phase_)) + "]";
    const bool emergency = threat.workerRush > .30 || threat.combatEnemiesNearMain >= 2 ||
        (threat.immediateGround > .60 && threat.enemiesNearMain > 0);
    if (phase_ == AllInPhase::transition) {
        protectObservedDtBreach(plan, state);
        // Native counters resume, with an explicit two-base recovery floor.
        plan.maximumBases = std::max(plan.maximumBases, 2);
        if (!emergency && plan.posture != Posture::recover) {
            if (!plan.expansionTarget.valid()) plan.expansionTarget = recoveryExpansion(state);
            plan.desiredBases = std::max(plan.desiredBases, 2);
            plan.desiredWorkers = std::max(plan.desiredWorkers, 32);
            plan.sustainEconomy = true;
            // Preserve the funded Nexus checkpoint instead of demoting it
            // behind an unbounded reinforcement queue during recovery.
            if (plan.expansionTarget.valid()) plan.prioritizeReinforcements = false;
            goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 128);
            goal(plan, GoalKind::train, UnitKind::probe, plan.desiredWorkers, 106);
            if (army >= 4) { plan.posture = Posture::attack; plan.minimumAttackSize = 4; }
        }
        return;
    }
    const auto original = plan.goals;
    const auto nativeDetectionRequired = plan.requireMobileDetection;
    const auto inheritedDetection = std::ranges::any_of(original, [](const ProductionGoal& prior) {
        return prior.goal == GoalKind::detect ||
               (prior.blocking && prior.target == UnitKind::observer);
    });
    plan.goals.clear();
    // Explicit fulfilled demands retire remembered optional tech/cannon goals.
    for (const auto& prior : original) {
        if (prior.goal == GoalKind::build || prior.goal == GoalKind::detect || prior.goal == GoalKind::expand)
            goal(plan, prior.goal, prior.target, count(state, prior.target), 0, false);
    }
    plan.desiredBases = plan.maximumBases = 1;
    plan.expansionTarget = {-1, -1};
    plan.desiredWorkers = p.workers;
    plan.desiredGasWorkers = zealot ? 0 : 3;
    plan.harassmentDrops = 0;
    plan.sustainEconomy = false;
    // This mode demotes optional tech and injects endless defensive units.
    // The opening already provides its own production priorities and caps.
    plan.prioritizeReinforcements = emergency;
    plan.minimumAttackSize = dt ? 2 : p.ready;
    plan.attackThreshold = 1.10;
    plan.posture = emergency ? Posture::defend : phase_ == AllInPhase::pressure ? Posture::attack : Posture::hold;
    const auto workers = count(state, UnitKind::probe);
    const auto gates = count(state, UnitKind::gateway);
    const auto core = count(state, UnitKind::cyberneticsCore, true);
    const auto dtChainFunded = count(state, UnitKind::templarArchives) > 0;
    plan.composition = {{UnitKind::zealot, zealot ? 1.0 : dt ? 0.0 : .15},
                        {UnitKind::dragoon, zealot || dt ? 0.0 : .85},
                        {UnitKind::darkTemplar, dt && dtChainFunded ? 1.0 : 0.0}};
    goal(plan, GoalKind::train, UnitKind::probe, p.workers, 105, false);
    if (workers >= 8) goal(plan, GoalKind::build, UnitKind::pylon, 1, 130);
    if (workers >= 10) goal(plan, GoalKind::build, UnitKind::gateway, 1, 126);
    if (zealot) {
        if (workers >= 11 && gates >= 1) goal(plan, GoalKind::build, UnitKind::gateway, 2, 124);
        if (gates >= 2) goal(plan, GoalKind::train, UnitKind::zealot,
                           count(state, UnitKind::zealot) + (core ? 1 : 2), core ? 117 : 120);
        // Ladder two-Gate winners add gas/Core behind the first Zealot wave,
        // rather than repeatedly feeding slow Zealots into ranged defenders.
        if (ready >= 3 || phase_ == AllInPhase::pressure || count(state, UnitKind::assimilator)) {
            goal(plan, GoalKind::build, UnitKind::assimilator, 1, 121);
            if (count(state, UnitKind::assimilator)) {
                plan.desiredGasWorkers = 3;
                goal(plan, GoalKind::build, UnitKind::cyberneticsCore, 1, 123);
            }
            if (core) {
                plan.composition = {{UnitKind::zealot, .4}, {UnitKind::dragoon, .6}};
                goal(plan, GoalKind::train, UnitKind::dragoon, count(state, UnitKind::dragoon) + 2, 120);
                plan.goals.push_back({GoalKind::upgrade, UnitKind::unknown, 1, 118, false,
                    "range behind ladder Zealot pressure", TechnologyKind::singularityCharge});
            }
        }
    } else {
        if (workers >= 11 && gates >= 1) goal(plan, GoalKind::build, UnitKind::assimilator, 1, 125);
        if (count(state, UnitKind::assimilator) >= 1 && gates >= 1)
            goal(plan, GoalKind::build, UnitKind::cyberneticsCore, 1, 123);
        if (core) {
            goal(plan, GoalKind::train, UnitKind::dragoon,
                 dt ? 2 : count(state, UnitKind::dragoon) + 2, 120);
            if (workers >= 16 && (!dt || dtChainFunded))
                goal(plan, GoalKind::build, UnitKind::gateway, 2, 121);
            if (!dt && workers >= 18 && gates >= 2 && count(state, UnitKind::dragoon) >= 2)
                goal(plan, GoalKind::build, UnitKind::gateway, p.gates, 119);
            if (!dt) plan.goals.push_back({GoalKind::upgrade, UnitKind::unknown, 1, 118, false,
                "range for ladder Dragoon pressure", TechnologyKind::singularityCharge});
            if (dt && (count(state, UnitKind::dragoon) >= 2 || count(state, UnitKind::citadelOfAdun))) {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 122);
                if (count(state, UnitKind::citadelOfAdun, true))
                    goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 122);
                if (count(state, UnitKind::templarArchives, true))
                    goal(plan, GoalKind::train, UnitKind::darkTemplar, count(state, UnitKind::darkTemplar) + 2, 124);
            }
        }
    }
    // Preserve native safety commitments across the opening's goal reset.
    // Only blocking/native demand survives; optional Observer scouting does not.
    for (const auto& prior : original) {
        if (prior.goal == GoalKind::detect && prior.target == UnitKind::observer) {
            auto detector = prior;
            detector.goal = GoalKind::train;
            plan.goals.push_back(std::move(detector));
        } else if (prior.blocking && prior.target == UnitKind::observer) {
            plan.goals.push_back(prior);
        }
    }
    // Direct evidence also retains the detector obligation during an all-in.
    const auto observedCloak = std::ranges::any_of(state.enemy.units, [&state](const UnitSnapshot& unit) {
        const bool recent = unit.visible || (state.frame >= unit.lastSeen &&
            state.frame - unit.lastSeen <= 15 * 24);
        if (!recent) return false;
        return ((unit.cloaked || unit.burrowed) &&
                (unit.groundWeapon.damage > 0 || unit.airWeapon.damage > 0)) ||
               unit.kind == UnitKind::darkTemplar || unit.kind == UnitKind::lurker ||
               unit.kind == UnitKind::spiderMine;
    });
    plan.requireMobileDetection = nativeDetectionRequired || inheritedDetection || observedCloak;
    if (plan.requireMobileDetection) {
        plan.desiredGasWorkers = std::max(plan.desiredGasWorkers, 3);
        for (auto& prior : plan.goals) {
            if (prior.goal == GoalKind::train && prior.target == UnitKind::observer && prior.blocking)
                prior.priority = std::max(prior.priority, 128);
        }
        const bool assimilatorDemand = std::ranges::any_of(plan.goals,
            [](const ProductionGoal& prior) {
                return prior.goal == GoalKind::build && prior.target == UnitKind::assimilator &&
                       prior.blocking;
            });
        if (count(state, UnitKind::assimilator) == 0 && !assimilatorDemand)
            goal(plan, GoalKind::build, UnitKind::assimilator, 1, 129,
                 "fund required mobile detection");
        const bool detectorDemand = std::ranges::any_of(plan.goals,
            [](const ProductionGoal& prior) {
                return prior.goal == GoalKind::train && prior.target == UnitKind::observer &&
                       prior.blocking;
            });
        if (!detectorDemand)
            goal(plan, GoalKind::train, UnitKind::observer, 1, 128,
                 "complete required mobile detection");
    }
    protectObservedDtBreach(plan, state);
    if (emergency) {
        for (const auto& prior : original)
            if (prior.target == UnitKind::photonCannon || prior.target == UnitKind::forge ||
                prior.target == UnitKind::shieldBattery) plan.goals.push_back(prior);
    }
}
}  // namespace protodd
