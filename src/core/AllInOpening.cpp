#include "protodd/AllInOpening.hpp"

#include <algorithm>
#include <array>

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
struct Profile { int workers; int gates; int ready; Frame window; Frame deadline; };
Position recoveryExpansion(const GameState& state) {
    const BaseSnapshot* best = nullptr;
    for (const auto& base : state.bases) {
        if (base.ownerId >= 0 || base.island || base.startLocation || base.mineralsRemaining <= 0 ||
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
        case AllInBuild::twoGateZealot: return {14, 2, 4, 120 * 24, 9000};
        case AllInBuild::threeGateDragoon: return {20, 3, 4, 120 * 24, 12000};
        case AllInBuild::fourGateDragoon: return {20, 4, 6, 150 * 24, 13200};
        case AllInBuild::darkTemplar: return {21, 3, 2, 60 * 24, 12000};
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
    build_ = build; phase_ = AllInPhase::assemble; launch_ = -1; peakArmy_ = 0; reason_ = "none";
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
        else if (launch_ >= 0 && state.frame - launch_ >= p.window) {
            phase_ = AllInPhase::transition; reason_ = "pressure-window";
        } else if (launch_ >= 0 && state.frame - launch_ >= 24 * 12 &&
                   peakArmy_ >= p.ready && army <= std::max(1, peakArmy_ / 3)) {
            phase_ = AllInPhase::transition; reason_ = "army-loss";
        } else if (ready >= p.ready && launch_ < 0) {
            phase_ = AllInPhase::pressure; launch_ = state.frame;
        }
    }
    plan.name += " [" + std::string(allInBuildName(build_)) + ":" +
                 std::string(allInPhaseName(phase_)) + "]";
    const bool emergency = threat.workerRush > .30 || threat.combatEnemiesNearMain >= 2 ||
        (threat.immediateGround > .60 && threat.enemiesNearMain > 0);
    if (phase_ == AllInPhase::transition) {
        // Native counters resume, with an explicit two-base recovery floor.
        plan.maximumBases = std::max(plan.maximumBases, 2);
        if (!emergency && plan.posture != Posture::recover) {
            if (!plan.expansionTarget.valid()) plan.expansionTarget = recoveryExpansion(state);
            plan.desiredBases = std::max(plan.desiredBases, 2);
            plan.desiredWorkers = std::max(plan.desiredWorkers, 32);
            plan.sustainEconomy = true;
            goal(plan, GoalKind::expand, UnitKind::nexus, plan.desiredBases, 128);
            goal(plan, GoalKind::train, UnitKind::probe, plan.desiredWorkers, 106);
            if (army >= 4) { plan.posture = Posture::attack; plan.minimumAttackSize = 4; }
        }
        return;
    }
    const auto original = plan.goals;
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
    plan.prioritizeReinforcements = true;
    plan.minimumAttackSize = dt ? 4 : p.ready;
    plan.attackThreshold = 1.10;
    plan.posture = emergency ? Posture::defend : phase_ == AllInPhase::pressure ? Posture::attack : Posture::hold;
    const auto workers = count(state, UnitKind::probe);
    const auto gates = count(state, UnitKind::gateway);
    const auto core = count(state, UnitKind::cyberneticsCore, true);
    plan.composition = {{UnitKind::zealot, zealot ? 1.0 : .15},
                        {UnitKind::dragoon, zealot ? 0.0 : dt ? .45 : .85},
                        {UnitKind::darkTemplar, dt ? .4 : 0.0}};
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
            goal(plan, GoalKind::train, UnitKind::dragoon, count(state, UnitKind::dragoon) + 2, 120);
            if (workers >= 16) goal(plan, GoalKind::build, UnitKind::gateway, 2, 121);
            if (workers >= 18 && gates >= 2 && count(state, UnitKind::dragoon) >= 2)
                goal(plan, GoalKind::build, UnitKind::gateway, p.gates, 119);
            if (!dt) plan.goals.push_back({GoalKind::upgrade, UnitKind::unknown, 1, 118, false,
                "range for ladder Dragoon pressure", TechnologyKind::singularityCharge});
            if (dt && count(state, UnitKind::dragoon) >= 2) {
                goal(plan, GoalKind::build, UnitKind::citadelOfAdun, 1, 122);
                if (count(state, UnitKind::citadelOfAdun, true))
                    goal(plan, GoalKind::build, UnitKind::templarArchives, 1, 122);
                if (count(state, UnitKind::templarArchives, true))
                    goal(plan, GoalKind::train, UnitKind::darkTemplar, count(state, UnitKind::darkTemplar) + 2, 124);
            }
        }
    }
    // Only observed cloak threats justify interrupting the opening for detection.
    const auto observedCloak = std::ranges::any_of(state.enemy.units, [](const UnitSnapshot& unit) {
        return unit.cloaked || unit.burrowed || unit.kind == UnitKind::darkTemplar ||
               unit.kind == UnitKind::lurker || unit.kind == UnitKind::spiderMine;
    });
    plan.requireMobileDetection = observedCloak;
    if (observedCloak) {
        plan.desiredGasWorkers = 3;
        goal(plan, GoalKind::detect, UnitKind::observer, 1, 128);
    }
    if (emergency) {
        for (const auto& prior : original)
            if (prior.target == UnitKind::photonCannon || prior.target == UnitKind::forge ||
                prior.target == UnitKind::shieldBattery) plan.goals.push_back(prior);
    }
}
}  // namespace protodd
