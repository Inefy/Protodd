#include "protodd/Workers.hpp"

#include <algorithm>
#include <iostream>
#include <string_view>
#include <utility>
#include <vector>

using namespace protodd;

namespace {
UnitSnapshot unit(const UnitId id, const UnitKind kind, const bool ours,
                  const Position position) {
    UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.position = result.lastPosition = position;
    result.ours = ours;
    result.completed = result.visible = result.detected = result.powered = true;
    result.hitPoints = result.maxHitPoints = 100;
    result.maxShields = 60;
    result.shields = 60;
    result.topSpeed = kind == UnitKind::zealot ? 3.15 : 4.0;
    result.groundWeapon = {.damage = 8, .cooldown = 22, .maxRange = 32,
                           .targetsGround = true};
    return result;
}

GameState twoBaseAttack() {
    GameState state;
    state.frame = 5 * 60 * 24;
    state.self.id = 1;
    state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    BaseSnapshot main;
    main.id = 1;
    main.center = {512, 512};
    main.mineralLine = {560, 512};
    main.ownerId = state.self.id;
    main.mineralsRemaining = 8000;
    BaseSnapshot natural;
    natural.id = 2;
    natural.center = {2300, 512};
    natural.mineralLine = {2348, 512};
    natural.ownerId = state.self.id;
    natural.mineralsRemaining = 8000;
    state.bases = {main, natural};

    state.self.units.push_back(unit(1, UnitKind::nexus, true, main.center));
    state.self.units.push_back(unit(2, UnitKind::nexus, true, natural.center));
    for (int id = 10; id < 20; ++id)
        state.self.units.push_back(unit(id, UnitKind::probe, true,
                                       {480 + (id % 4) * 20, 550}));
    for (int id = 20; id < 30; ++id)
        state.self.units.push_back(unit(id, UnitKind::probe, true,
                                       {2280 + (id % 4) * 20, 550}));

    auto mainAttacker = unit(100, UnitKind::zealot, false, {620, 512});
    auto naturalAttacker = unit(101, UnitKind::zealot, false, {2410, 512});
    state.enemy.units = {mainAttacker, naturalAttacker};
    return state;
}

void addScreen(GameState& state, const Position first, const Position second) {
    state.self.units.push_back(unit(30, UnitKind::zealot, true, first));
    state.self.units.push_back(unit(31, UnitKind::zealot, true, second));
}

int defenders(const std::vector<WorkerAssignment>& assignments, const UnitId target) {
    return static_cast<int>(std::ranges::count_if(
        assignments, [target](const WorkerAssignment& assignment) {
            return assignment.job == WorkerJob::defend && assignment.targetUnit == target;
        }));
}

int assignmentsNear(const std::vector<WorkerAssignment>& assignments,
                    const GameState& state, const WorkerJob job,
                    const bool atMain) {
    return static_cast<int>(std::ranges::count_if(
        assignments, [&state, job, atMain](const WorkerAssignment& assignment) {
            if (assignment.job != job) return false;
            const auto worker = std::ranges::find(state.self.units, assignment.worker,
                                                   &UnitSnapshot::id);
            return worker != state.self.units.end() &&
                   ((worker->position.x < 1000) == atMain);
        }));
}

bool check(const bool condition, const std::string_view message) {
    if (!condition) std::cerr << "FAIL: " << message << '\n';
    return condition;
}

bool impairedScreenStillRequestsMilitia(
    const std::string_view label,
    const auto& impair) {
    auto state = twoBaseAttack();
    state.enemy.units = {state.enemy.units.back()};
    addScreen(state, {2350, 512}, {2370, 512});
    impair(state.self.units.back());
    const auto assignments = WorkerManager{}.assign(state, {}, InfluenceMap{});
    return check(defenders(assignments, 101) > 0, label);
}
}

int main() {
    auto state = twoBaseAttack();
    addScreen(state, {550, 512}, {570, 512});
    const auto mainScreenAssignments = WorkerManager{}.assign(state, {}, InfluenceMap{});
    if (!check(defenders(mainScreenAssignments, 100) == 0 &&
                   defenders(mainScreenAssignments, 101) == 3,
               "a main-base screen suppresses only main militia; the natural receives three defenders")) {
        return 1;
    }
    const auto naturalWorkersDefend = std::ranges::all_of(
        mainScreenAssignments, [&state](const WorkerAssignment& assignment) {
            if (assignment.job != WorkerJob::defend) return true;
            const auto worker = std::ranges::find(state.self.units, assignment.worker,
                                                  &UnitSnapshot::id);
            return worker != state.self.units.end() && worker->position.x > 2000;
        });
    if (!check(naturalWorkersDefend,
               "natural defense does not pull workers across the map from the main")) {
        return 1;
    }
    const auto naturalWorkersEvacuateLocally = std::ranges::all_of(
        mainScreenAssignments, [&state](const WorkerAssignment& assignment) {
            if (assignment.job != WorkerJob::evacuate) return true;
            const auto worker = std::ranges::find(state.self.units, assignment.worker,
                                                  &UnitSnapshot::id);
            return worker != state.self.units.end() && worker->position.x > 2000;
        });
    if (!check(naturalWorkersEvacuateLocally,
               "a natural attack evacuates only workers assigned to the natural mineral line")) {
        return 1;
    }

    auto swappedState = twoBaseAttack();
    addScreen(swappedState, {2350, 512}, {2370, 512});
    const auto naturalScreenAssignments = WorkerManager{}.assign(
        swappedState, {}, InfluenceMap{});
    if (!check(defenders(naturalScreenAssignments, 101) == 0 &&
                   defenders(naturalScreenAssignments, 100) == 3,
               "moving the two-unit screen to the natural changes only the two local militia decisions")) {
        return 1;
    }
    const auto mainWorkersDefend = std::ranges::all_of(
        naturalScreenAssignments, [&swappedState](const WorkerAssignment& assignment) {
            if (assignment.job != WorkerJob::defend) return true;
            const auto worker = std::ranges::find(swappedState.self.units, assignment.worker,
                                                  &UnitSnapshot::id);
            return worker != swappedState.self.units.end() && worker->position.x < 1000;
        });
    if (!check(mainWorkersDefend,
               "main defense does not pull workers across the map from the natural")) {
        return 1;
    }
    const auto naturalScreenKeepsEvacuationLocal = std::ranges::all_of(
        naturalScreenAssignments, [&swappedState](const WorkerAssignment& assignment) {
            if (assignment.job != WorkerJob::evacuate) return true;
            const auto worker = std::ranges::find(swappedState.self.units, assignment.worker,
                                                  &UnitSnapshot::id);
            return worker != swappedState.self.units.end() && worker->position.x < 1000;
        });
    if (!check(naturalScreenKeepsEvacuationLocal,
               "moving the screen to the natural keeps evacuation decisions local to the main")) {
        return 1;
    }

    if (!impairedScreenStillRequestsMilitia(
            "a wounded unit cannot erase the natural emergency", [](UnitSnapshot& unit) {
                unit.hitPoints = 20;
                unit.shields = 0;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "a disabled unit cannot erase the natural emergency", [](UnitSnapshot& unit) {
                unit.disabled = true;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "a loaded unit cannot erase the natural emergency", [](UnitSnapshot& unit) {
                unit.loaded = true;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "a unit without a ground weapon cannot erase the natural emergency",
            [](UnitSnapshot& unit) { unit.groundWeapon = {}; }) ||
        !impairedScreenStillRequestsMilitia(
            "an empty Reaver cannot erase the natural emergency", [](UnitSnapshot& unit) {
                unit.kind = UnitKind::reaver;
                unit.ammo = 0;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "an invincible unit is not credited as a local defender", [](UnitSnapshot& unit) {
                unit.invincible = true;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "a hallucinated unit is not credited as a local defender", [](UnitSnapshot& unit) {
                unit.hallucination = true;
            }) ||
        !impairedScreenStillRequestsMilitia(
            "a unit with no movement speed cannot erase the natural emergency",
            [](UnitSnapshot& unit) { unit.topSpeed = 0.0; }) ||
        !impairedScreenStillRequestsMilitia(
            "a defender outside the four-second response window cannot erase the emergency",
            [](UnitSnapshot& unit) { unit.position = {2000, 512}; })) {
        return 1;
    }

    auto ineffectiveState = twoBaseAttack();
    ineffectiveState.enemy.units = {ineffectiveState.enemy.units.back()};
    addScreen(ineffectiveState, {2350, 512}, {2370, 512});
    for (auto& own : ineffectiveState.self.units) {
        if (own.id < 30) continue;
        own.hitPoints = 20;
        own.shields = 0;
    }
    const auto ineffectiveAssignments = WorkerManager{}.assign(
        ineffectiveState, {}, InfluenceMap{});
    if (!check(defenders(ineffectiveAssignments, 101) == 3,
               "two wounded units cannot erase the natural militia emergency")) {
        return 1;
    }

    auto blockedState = twoBaseAttack();
    blockedState.enemy.units = {blockedState.enemy.units.back()};
    addScreen(blockedState, {2300, 512}, {2320, 512});
    for (auto& own : blockedState.self.units) {
        if (own.kind == UnitKind::probe && own.position.x > 2000)
            own.position = {2300 + (own.id % 4) * 20, 800};
    }
    constexpr int mapWidth = 80;
    constexpr int mapHeight = 40;
    constexpr int wallColumn = 74;
    std::vector<std::uint8_t> walkable(mapWidth * mapHeight, 1U);
    for (auto y = 0; y < mapHeight; ++y)
        walkable[static_cast<std::size_t>(y * mapWidth + wallColumn)] = 0U;
    const NavigationGrid splitMap(mapWidth, mapHeight, 32, std::move(walkable));
    const auto blockedAssignments = WorkerManager{}.assign(
        blockedState, {}, InfluenceMap{}, {}, false, false, &splitMap);
    if (!check(defenders(blockedAssignments, 101) == 3,
               "nearby but terrain-separated units cannot suppress reachable worker defense")) {
        return 1;
    }

    auto effectiveState = twoBaseAttack();
    effectiveState.enemy.units = {effectiveState.enemy.units.back()};
    addScreen(effectiveState, {2350, 512}, {2370, 512});
    const auto effectiveAssignments = WorkerManager{}.assign(
        effectiveState, {}, InfluenceMap{});
    if (!check(defenders(effectiveAssignments, 101) == 0,
               "two healthy, armed, nearby units prevent unnecessary Probe militia")) {
        return 1;
    }

    auto evacuationState = twoBaseAttack();
    evacuationState.frame = 5 * 60 * 24;
    evacuationState.enemy.units = {unit(200, UnitKind::zergling, false, {512, 512})};
    for (auto& own : evacuationState.self.units) {
        if (own.kind == UnitKind::probe && own.position.x < 1000) own.underAttack = true;
    }
    InfluenceMap evacuationInfluence;
    evacuationInfluence.resize(4096, 2560);
    evacuationInfluence.update(evacuationState);
    std::vector<std::uint8_t> openTerrain(128U * 80U, 1U);
    const NavigationGrid openMap(128, 80, 32, std::move(openTerrain));
    WorkerManager evacuationWorkers;
    const UnitId leasedBuilders[]{10, 20};
    WorkerManager builderSafetyWorkers;
    const auto builderSafety = builderSafetyWorkers.assign(
        evacuationState, {}, evacuationInfluence, leasedBuilders, false, false, &openMap);
    const auto jobFor = [&builderSafety](const UnitId id) {
        const auto found = std::ranges::find(builderSafety, id, &WorkerAssignment::worker);
        return found == builderSafety.end() ? WorkerJob::idle : found->job;
    };
    if (!check(jobFor(10) == WorkerJob::evacuate,
               "an endangered leased builder enters the emergency escape path") ||
        !check(jobFor(20) == WorkerJob::build,
               "a safe leased builder keeps construction ownership")) {
        return 1;
    }

    auto targetedAttackState = twoBaseAttack();
    auto attacker = unit(201, UnitKind::zergling, false, {512, 512});
    attacker.orderTargetId = 10;
    targetedAttackState.enemy.units = {attacker};
    InfluenceMap targetedInfluence;
    targetedInfluence.resize(4096, 2560);
    targetedInfluence.update(targetedAttackState);
    std::vector<std::uint8_t> targetedTerrain(128U * 80U, 1U);
    const NavigationGrid targetedMap(128, 80, 32, std::move(targetedTerrain));
    WorkerManager targetedWorkers;
    const UnitId targetedLeases[]{10, 20};
    const auto targetedAssignments = targetedWorkers.assign(
        targetedAttackState, {}, targetedInfluence, targetedLeases,
        false, false, &targetedMap);
    const auto targetedJobFor = [&](const UnitId id) {
        const auto found = std::ranges::find(
            targetedAssignments, id, &WorkerAssignment::worker);
        return found == targetedAssignments.end() ? WorkerJob::idle : found->job;
    };
    if (!check(targetedJobFor(10) == WorkerJob::evacuate,
               "a hostile unit targeting a travelling leased builder triggers escape") ||
        !check(targetedJobFor(20) == WorkerJob::build,
               "a remote safe builder keeps its construction lease during local attack")) {
        return 1;
    }

    const auto immediateEscape = evacuationWorkers.assign(
        evacuationState, {}, evacuationInfluence, {}, false, false, &openMap);
    if (!check(assignmentsNear(immediateEscape, evacuationState, WorkerJob::evacuate, true) == 10 &&
                   assignmentsNear(immediateEscape, evacuationState, WorkerJob::minerals, true) == 0,
               "immediate local danger overrides the four-Probe mining floor") ||
        !check(assignmentsNear(immediateEscape, evacuationState, WorkerJob::minerals, false) == 10 &&
                   assignmentsNear(immediateEscape, evacuationState, WorkerJob::evacuate, false) == 0,
               "evacuation keeps the protected natural pocket mining")) {
        return 1;
    }
    for (const auto& assignment : immediateEscape) {
        if (assignment.job != WorkerJob::evacuate) continue;
        const auto worker = std::ranges::find(evacuationState.self.units, assignment.worker,
                                               &UnitSnapshot::id);
        if (worker == evacuationState.self.units.end() || worker->position.x >= 1000) continue;
        if (!check(openMap.walkable(assignment.targetPosition) &&
                       openMap.lineWalkable(worker->position, assignment.targetPosition) &&
                       distanceSquared(assignment.targetPosition,
                                       evacuationState.enemy.units.front().position) >
                           distanceSquared(worker->position,
                                           evacuationState.enemy.units.front().position),
                   "an endangered Probe receives a reachable evacuation waypoint")) {
            return 1;
        }
    }

    for (auto& own : evacuationState.self.units) own.underAttack = false;
    evacuationState.frame++;
    evacuationInfluence.update(evacuationState);
    const auto persistentDanger = evacuationWorkers.assign(
        evacuationState, {}, evacuationInfluence, {}, false, false, &openMap);
    if (!check(assignmentsNear(persistentDanger, evacuationState, WorkerJob::evacuate, true) == 10 &&
                   assignmentsNear(persistentDanger, evacuationState, WorkerJob::minerals, true) == 0,
               "the same attacker cannot trigger a flee-to-mine oscillation")) {
        return 1;
    }

    evacuationState.enemy.units.clear();
    evacuationState.frame++;
    evacuationInfluence.update(evacuationState);
    const auto quietHold = evacuationWorkers.assign(
        evacuationState, {}, evacuationInfluence, {}, false, false, &openMap);
    if (!check(assignmentsNear(quietHold, evacuationState, WorkerJob::evacuate, true) == 10 &&
                   assignmentsNear(quietHold, evacuationState, WorkerJob::minerals, true) == 0,
               "reentry waits through the configured quiet period after danger clears")) {
        return 1;
    }
    evacuationState.frame += 49;
    const auto resumedMining = evacuationWorkers.assign(
        evacuationState, {}, evacuationInfluence, {}, false, false, &openMap);
    const auto resumedIncome =
        assignmentsNear(resumedMining, evacuationState, WorkerJob::minerals, true) +
        assignmentsNear(resumedMining, evacuationState, WorkerJob::transfer, true);
    if (!check(resumedIncome == 10 &&
                   assignmentsNear(resumedMining, evacuationState, WorkerJob::evacuate, true) == 0,
               "workers resume income after the local danger clears")) {
        return 1;
    }
    return 0;
}
