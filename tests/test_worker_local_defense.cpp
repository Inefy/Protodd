#include "protodd/Workers.hpp"
#include "protodd/WorkerCargoRecovery.hpp"

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


bool unpoweredCannonsDoNotDisruptWorkers() {
    auto state = twoBaseAttack();
    auto cannon = unit(200, UnitKind::photonCannon, false, {620, 512});
    cannon.topSpeed = 0.0;
    cannon.groundWeapon.maxRange = 224;
    cannon.powered = false;
    state.enemy.units = {cannon};
    auto passed = true;
    const auto inactive = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(inactive, state, WorkerJob::evacuate, true) == 0 &&
                        assignmentsNear(inactive, state, WorkerJob::minerals, true) == 10,
                    "an unpowered Cannon in weapon range leaves the mineral line mining");
    const UnitId builder[]{10};
    const auto building = WorkerManager{}.assign(state, {}, InfluenceMap{}, builder);
    passed &= check(std::ranges::any_of(building, [](const WorkerAssignment& assignment) {
                        return assignment.worker == 10 && assignment.job == WorkerJob::build;
                    }), "an unpowered Cannon cannot cancel a reserved builder");
    state.enemy.units.front().powered = true;
    WorkerManager remembered;
    const auto powered = remembered.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(powered, state, WorkerJob::evacuate, true) == 10,
                    "restoring Cannon power still evacuates workers in range");
    state.enemy.units.front().powered = false;
    state.frame += 25;
    const auto cleared = remembered.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(cleared, state, WorkerJob::evacuate, true) == 0 &&
                        assignmentsNear(cleared, state, WorkerJob::minerals, true) == 10,
                    "lost Cannon power clears retained danger after the existing quiet hold");
    auto damaged = state;
    std::ranges::find(damaged.self.units, 10, &UnitSnapshot::id)->underAttack = true;
    const auto escape = WorkerManager{}.assign(damaged, {}, InfluenceMap{});
    passed &= check(std::ranges::any_of(escape, [](const WorkerAssignment& assignment) {
                        return assignment.worker == 10 && assignment.job == WorkerJob::evacuate;
                    }), "actual incoming damage still evacuates a Probe beside an unpowered Cannon");
    auto unfinished = state;
    unfinished.enemy.units.front().completed = false;
    passed &= check(defenders(WorkerManager{}.assign(unfinished, {}, InfluenceMap{}), 200) > 0,
                    "unpowered unfinished Cannons still receive construction-denial militia");
    auto ordinary = state;
    ordinary.enemy.units.front().kind = UnitKind::marine;
    passed &= check(assignmentsNear(WorkerManager{}.assign(ordinary, {}, InfluenceMap{}),
                                   ordinary, WorkerJob::evacuate, true) == 10,
                    "a unit that needs no Psi remains threatening regardless of its power flag");

    auto gas = state;
    gas.enemy.units.front().position = {1056, 512};
    gas.self.units.push_back(unit(3, UnitKind::assimilator, true, {800, 512}));
    StrategicPlan gasPlan;
    gasPlan.desiredGasWorkers = 3;
    const auto inactiveGas = WorkerManager{}.assign(gas, gasPlan, InfluenceMap{});
    passed &= check(std::ranges::count(inactiveGas, WorkerJob::gas,
                                      &WorkerAssignment::job) == 3,
                    "an unpowered Cannon cannot deny an otherwise safe refinery");
    gas.enemy.units.front().powered = true;
    passed &= check(std::ranges::count(WorkerManager{}.assign(gas, gasPlan, InfluenceMap{}),
                                      WorkerJob::gas, &WorkerAssignment::job) == 0,
                    "a powered Cannon still denies the exposed refinery");

    auto abandoned = state;
    abandoned.bases.front().ownerId = abandoned.enemy.id;
    std::erase_if(abandoned.self.units, [](const UnitSnapshot& own) { return own.id == 1; });
    abandoned.enemy.units.front().position = {1000, 512};
    const auto safeAbandoned = WorkerManager{}.assign(abandoned, {}, InfluenceMap{}, {}, true);
    passed &= check(assignmentsNear(safeAbandoned, abandoned, WorkerJob::evacuate, true) == 0,
                    "an unpowered Cannon does not label an abandoned mineral line hostile");
    abandoned.enemy.units.front().powered = true;
    const auto hostileAbandoned = WorkerManager{}.assign(abandoned, {}, InfluenceMap{}, {}, true);
    passed &= check(assignmentsNear(hostileAbandoned, abandoned, WorkerJob::evacuate, true) == 10,
                    "a powered Cannon still clears the hostile abandoned mineral line");
    return passed;
}


bool flyingGroundAttackersStillThreatenWorkers() {
    auto state = twoBaseAttack();
    auto mutalisk = unit(200, UnitKind::mutalisk, false, {620, 512});
    mutalisk.flying = true;
    mutalisk.topSpeed = 6.0;
    mutalisk.groundWeapon.maxRange = 96;
    state.enemy.units = {mutalisk};
    auto passed = true;
    const auto exposed = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(exposed, state, WorkerJob::evacuate, true) == 10 &&
                        defenders(exposed, 200) == 0,
                    "a flying ground attacker evacuates exposed Probes without assigning melee militia");
    addScreen(state, {600, 512}, {610, 512});
    const auto groundOnly = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(groundOnly, state, WorkerJob::evacuate, true) == 10,
                    "two ground-only defenders cannot cover workers from a Mutalisk");
    for (auto& own : state.self.units) {
        if (own.kind != UnitKind::zealot) continue;
        own.kind = UnitKind::dragoon;
        own.airWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                         .targetsAir = true};
    }
    const auto airScreen = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(airScreen, state, WorkerJob::evacuate, true) == 0 &&
                        assignmentsNear(airScreen, state, WorkerJob::minerals, true) == 10,
                    "two ready anti-air defenders still keep the covered mineral line mining");
    auto pursuit = twoBaseAttack();
    mutalisk.position = {1000, 550};
    mutalisk.orderTargetId = 10;
    pursuit.enemy.units = {mutalisk};
    const UnitId builder[]{10};
    const auto targeted = WorkerManager{}.assign(pursuit, {}, InfluenceMap{}, builder);
    passed &= check(std::ranges::any_of(targeted, [](const WorkerAssignment& assignment) {
                        return assignment.worker == 10 && assignment.job == WorkerJob::evacuate;
                    }), "a Mutalisk pursuing a builder preserves early cancellation warning");
    pursuit.enemy.units.front().kind = UnitKind::corsair;
    pursuit.enemy.units.front().groundWeapon = {};
    const auto harmlessToGround = WorkerManager{}.assign(pursuit, {}, InfluenceMap{}, builder);
    passed &= check(std::ranges::any_of(harmlessToGround, [](const WorkerAssignment& assignment) {
                        return assignment.worker == 10 && assignment.job == WorkerJob::build;
                    }), "an air-only attacker cannot cancel a grounded Probe's construction");
    return passed;
}

bool stationaryThreatKeepsItsWeaponRange() {
    auto state = twoBaseAttack();
    auto cannon = unit(200, UnitKind::photonCannon, false, {944, 512});
    cannon.topSpeed = 0.0;
    cannon.groundWeapon.maxRange = 224;
    state.enemy.units = {cannon};
    const auto outsideRange = WorkerManager{}.assign(state, {}, InfluenceMap{});
    auto passed = check(
        assignmentsNear(outsideRange, state, WorkerJob::minerals, true) +
                assignmentsNear(outsideRange, state, WorkerJob::transfer, true) == 10 &&
            assignmentsNear(outsideRange, state, WorkerJob::evacuate, true) == 0,
        "a stationary Cannon outside weapon range cannot force the mineral line to evacuate");
    state.enemy.units.front().position = {620, 512};
    const auto insideRange = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(
        assignmentsNear(insideRange, state, WorkerJob::evacuate, true) == 10 &&
            assignmentsNear(insideRange, state, WorkerJob::evacuate, false) == 0,
        "a stationary Cannon in range still evacuates the exposed mineral line");
    state.enemy.units.front().position = {800, 512};
    state.enemy.units.front().kind = UnitKind::dragoon;
    state.enemy.units.front().topSpeed = 4.0;
    state.enemy.units.front().groundWeapon.maxRange = 192;
    const auto approaching = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(
        assignmentsNear(approaching, state, WorkerJob::evacuate, true) > 0,
        "a mobile ranged attacker can still trigger evacuation before entering range");
    return passed;
}

bool staticScreensStayLocal() {
    auto passed = true;
    for (const auto kind : {UnitKind::photonCannon, UnitKind::shieldBattery}) {
        auto state = twoBaseAttack();
        state.frame = 4 * 60 * 24;
        for (auto& enemy : state.enemy.units) {
            enemy.kind = UnitKind::marine;
            enemy.groundWeapon.maxRange = 128;
        }
        auto screen = unit(30, kind, true, state.bases.front().center);
        screen.topSpeed = 0.0;
        state.self.units.push_back(screen);
        const auto mainScreen = WorkerManager{}.assign(state, {}, InfluenceMap{});
        passed &= check(defenders(mainScreen, 100) == 0 && defenders(mainScreen, 101) == 1,
                        "a main static screen cannot suppress the natural's Marine militia");
        state.self.units.back().position = state.bases.back().center;
        const auto naturalScreen = WorkerManager{}.assign(state, {}, InfluenceMap{});
        passed &= check(defenders(naturalScreen, 100) == 1 && defenders(naturalScreen, 101) == 0,
                        "moving a static screen changes only the protected base's Marine response");
        for (const auto impairment : {0, 1, 2, 3, 4, 5}) {
            auto impaired = state;
            auto& defense = impaired.self.units.back();
            if (impairment == 0) defense.powered = false;
            if (impairment == 1) defense.completed = false;
            if (impairment == 2) defense.disabled = true;
            if (impairment == 3) defense.hallucination = true;
            if (impairment == 4) defense.position = {-1, -1};
            if (impairment == 5) defense.position = {3800, 512};
            const auto assignments = WorkerManager{}.assign(impaired, {}, InfluenceMap{});
            passed &= check(defenders(assignments, 100) == 1 && defenders(assignments, 101) == 1,
                            "an unavailable or distant static screen cannot suppress Marine militia");
        }
    }
    return passed;
}

bool workerScreensMustBeReadyToFight() {
    auto passed = impairedScreenStillRequestsMilitia(
        "a recharging fighter cannot erase the natural militia emergency",
        [](UnitSnapshot& defender) { defender.recharging = true; });
    auto state = twoBaseAttack();
    auto threat = unit(200, UnitKind::lurker, false, {2410, 512});
    threat.burrowed = true;
    threat.detected = false;
    threat.groundWeapon.maxRange = 192;
    state.enemy.units = {threat};
    addScreen(state, {2350, 512}, {2370, 512});
    for (auto& defender : state.self.units) {
        if (defender.id < 30) continue;
        defender.kind = UnitKind::dragoon;
        defender.groundWeapon.maxRange = 128;
    }
    const auto undetected = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(undetected, state, WorkerJob::evacuate, false) == 10 &&
                        assignmentsNear(undetected, state, WorkerJob::evacuate, true) == 0,
                    "fighters cannot protect workers from an untargetable burrowed threat");

    // A nearby detector is not proof that BWAPI currently permits attacking
    // this contact. Wait for the observed detection flag before granting cover.
    state.self.units.push_back(unit(40, UnitKind::observer, true, {2400, 512}));
    const auto pendingDetection = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(pendingDetection, state, WorkerJob::evacuate, false) == 10,
                    "a nearby detector cannot grant cover before the threat becomes detected");
    state.enemy.units.front().detected = true;
    const auto detected = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(detected, state, WorkerJob::evacuate, false) == 0,
                    "two ready fighters protect workers once the threat is detected");
    for (auto& defender : state.self.units) {
        if (defender.kind == UnitKind::dragoon) defender.recharging = true;
    }
    const auto recharging = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(recharging, state, WorkerJob::evacuate, false) == 10,
                    "battery recharge orders cannot grant a ready screen against ranged pressure");
    for (auto& defender : state.self.units) defender.recharging = false;
    auto& remembered = state.enemy.units.front();
    remembered.kind = UnitKind::dragoon;
    remembered.burrowed = false;
    remembered.visible = remembered.detected = false;
    remembered.lastSeen = state.frame - 6;
    const auto foggedOrdinaryThreat = WorkerManager{}.assign(state, {}, InfluenceMap{});
    passed &= check(assignmentsNear(foggedOrdinaryThreat, state, WorkerJob::evacuate, false) == 0,
                    "fog reconciliation alone does not remove cover against an ordinary recent threat");
    return passed;
}

bool targetedBuildersNeedImminentContact() {
    auto state = twoBaseAttack();
    state.frame = 2564;
    state.bases.front().center = state.self.units.front().position = {1056, 272};
    auto& first = *std::ranges::find(state.self.units, 10, &UnitSnapshot::id);
    auto& second = *std::ranges::find(state.self.units, 11, &UnitSnapshot::id);
    first.position = {953, 159};
    second.position = {907, 140};
    auto scout = unit(21, UnitKind::probe, false, {1326, 356});
    scout.topSpeed = 4.92;
    scout.groundWeapon.damage = 5;
    scout.orderTargetId = first.id;
    state.enemy.units = {scout};
    const UnitId builders[]{10, 11};
    const auto jobFor = [](const auto& assignments, const UnitId id) {
        return std::ranges::find(assignments, id, &WorkerAssignment::worker)->job;
    };
    WorkerManager manager;
    const auto distantOrder = manager.assign(state, {}, InfluenceMap{}, builders);
    auto passed = check(jobFor(distantOrder, 10) == WorkerJob::build &&
                            jobFor(distantOrder, 11) == WorkerJob::build,
                        "a distant scout's target order cannot cancel safe construction");
    state.frame += 24;
    state.enemy.units.front().orderTargetId = second.id;
    const auto changedTarget = manager.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(changedTarget, 10) == WorkerJob::build &&
                        jobFor(changedTarget, 11) == WorkerJob::build,
                    "a distant scout switching targets cannot alternate builder cancellations");
    first.underAttack = true;
    const auto actualDamage = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(actualDamage, 10) == WorkerJob::evacuate,
                    "actual attack pressure still overrides a builder lease immediately");
    first.underAttack = false;
    state.enemy.units.front().position = {940, 140};
    const auto closeTarget = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(closeTarget, 11) == WorkerJob::evacuate,
                    "a targeted builder escapes when the scout can contact it imminently");
    state.enemy.units.front().position = scout.position;
    state.enemy.units.front().kind = UnitKind::photonCannon;
    state.enemy.units.front().topSpeed = 0.0;
    state.enemy.units.front().groundWeapon.maxRange = 224;
    const auto outOfRangeStatic = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(outOfRangeStatic, 11) == WorkerJob::build,
                    "a stationary out-of-range target order cannot cancel construction");
    state.enemy.units.front().position = {1000, 140};
    const auto inRangeStatic = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(inRangeStatic, 11) == WorkerJob::evacuate,
                    "an in-range stationary attacker still triggers immediate escape");
    state.enemy.units = {scout};
    state.enemy.units.front().kind = UnitKind::zergling;
    state.enemy.units.front().topSpeed = 4.13;
    state.enemy.units.front().orderTargetId = second.id;
    const auto militaryPursuit = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(militaryPursuit, 11) == WorkerJob::evacuate,
                    "a military pursuer gives a targeted builder early warning before contact");
    state.enemy.units = {scout, unit(22, UnitKind::probe, false, {1310, 356}),
                               unit(23, UnitKind::probe, false, {1296, 356})};
    state.enemy.units.front().orderTargetId = second.id;
    const auto workerRush = WorkerManager{}.assign(state, {}, InfluenceMap{}, builders);
    passed &= check(jobFor(workerRush, 11) == WorkerJob::evacuate,
                    "a coordinated local worker rush preserves targeted builder warning time");
    return passed;
}
}

int main() {
    const auto airThreatPassed = flyingGroundAttackersStillThreatenWorkers();
    const auto inactiveThreatPassed = unpoweredCannonsDoNotDisruptWorkers();
    if (!airThreatPassed || !inactiveThreatPassed) return 1;
    const auto stationaryRangePassed = stationaryThreatKeepsItsWeaponRange();
    const auto staticLocalityPassed = staticScreensStayLocal();
    const auto readinessPassed = workerScreensMustBeReadyToFight();
    const auto targetedBuilderPassed = targetedBuildersNeedImminentContact();
    if (!stationaryRangePassed || !staticLocalityPassed || !readinessPassed ||
        !targetedBuilderPassed) return 1;

    // Macro runs between worker planning passes. Cancellation acknowledgement
    // must not make the evacuating Probe or its two defenders new builders.
    {
        auto state = twoBaseAttack();
        state.frame = 2870;
        auto drone = unit(200, UnitKind::drone, false, {600, 550});
        drone.orderTargetId = 10;
        state.enemy.units = {drone};
        WorkerManager manager;
        const UnitId builders[]{10};
        const auto assignments = manager.assign(state, {}, InfluenceMap{}, builders);
        const auto excluded = manager.constructionExclusions(state);
        const auto contains = [](const auto& values, const UnitId id) {
            return std::ranges::find(values, id) != values.end();
        };
        if (!check(contains(excluded, 10) && excluded.size() == 3 &&
                       defenders(assignments, 200) == 2 && !contains(excluded, 20),
                   "an evacuating builder and its defenders are unavailable for new construction")) return 1;
        state.frame += 6;
        if (!check(manager.constructionExclusions(state) == excluded,
                   "emergency construction exclusions survive between worker-planning passes")) return 1;
        auto reused = state;
        std::ranges::find(reused.self.units, 10, &UnitSnapshot::id)->firstSeen = state.frame;
        if (!check(!contains(manager.constructionExclusions(reused), 10),
                   "a new worker lifetime cannot inherit an old emergency exclusion")) return 1;
        std::erase_if(reused.self.units, [](const UnitSnapshot& worker) { return worker.id == 11; });
        if (!check(!contains(manager.constructionExclusions(reused), 11),
                   "a destroyed worker cannot remain in the construction exclusion list")) return 1;
        auto stale = state;
        stale.frame += 25;
        if (!check(manager.constructionExclusions(stale).empty(),
                   "missed worker refreshes cannot lock construction indefinitely")) return 1;
        auto rewind = state;
        rewind.frame = 0;
        if (!check(manager.constructionExclusions(rewind).empty(),
                   "a frame rewind cannot reuse another match's emergency assignments")) return 1;
        state.frame += 6;
        state.enemy.units.clear();
        static_cast<void>(manager.assign(state, {}, InfluenceMap{}));
        if (!check(manager.constructionExclusions(state) == std::vector<UnitId>{10},
                   "the short evacuation quiet hold stays protected while released defenders become available")) return 1;
        state.frame += 48;
        static_cast<void>(manager.assign(state, {}, InfluenceMap{}));
        if (!check(manager.constructionExclusions(state).empty(),
                   "ordinary mining assignments promptly restore construction eligibility")) return 1;
        state.enemy.units = {drone};
        state.bases.clear();
        std::erase_if(state.self.units, [](const UnitSnapshot& worker) {
            return worker.kind == UnitKind::nexus;
        });
        static_cast<void>(manager.assign(state, {}, InfluenceMap{}));
        if (!check(contains(manager.constructionExclusions(state), 10),
                   "the no-depot recovery branch also retains emergency construction exclusions")) return 1;
    }

    // A scout that leaves the owned base must release the militia. A Probe
    // still carrying its last mineral must also replace the persistent native
    // AttackUnit order, rather than chase the scout until it becomes idle.
    auto scoutState = twoBaseAttack();
    scoutState.frame = 2600;
    scoutState.enemy.units = {unit(200, UnitKind::probe, false, {600, 550})};
    scoutState.enemy.units.front().orderTargetId = 10;
    WorkerManager scoutDefense;
    const auto localScoutDefense = scoutDefense.assign(scoutState, {}, InfluenceMap{});
    if (!check(defenders(localScoutDefense, 200) == 2,
               "an attacking scout receives two local worker defenders")) return 1;
    scoutState.frame += 240;
    scoutState.enemy.units.front().position = {1500, 1200};
    const auto releasedScoutDefense = scoutDefense.assign(scoutState, {}, InfluenceMap{});
    auto cargoHandoffs = 0;
    for (const auto& prior : localScoutDefense) {
        if (prior.job != WorkerJob::defend) continue;
        const auto released = std::ranges::find(releasedScoutDefense, prior.worker,
                                                &WorkerAssignment::worker);
        if (released != releasedScoutDefense.end() &&
            released->job == WorkerJob::minerals &&
            shouldReturnWorkerCargo(released->job, true, false, true)) ++cargoHandoffs;
    }
    if (!check(cargoHandoffs == 2,
               "carrying defenders released to mining return cargo instead of chasing")) return 1;
    if (!check(!shouldReturnWorkerCargo(WorkerJob::minerals, true, false, false) &&
                   !shouldReturnWorkerCargo(WorkerJob::minerals, false, false, true) &&
                   !shouldReturnWorkerCargo(WorkerJob::defend, true, false, true) &&
                   !shouldReturnWorkerCargo(WorkerJob::evacuate, true, false, true) &&
                   !shouldReturnWorkerCargo(WorkerJob::scout, true, false, true),
               "cargo recovery preserves hauling, empty workers, and active safety/scouting jobs")) return 1;
    if (!check(shouldReturnWorkerCargo(WorkerJob::gas, true, false, true) &&
                   shouldReturnWorkerCargo(WorkerJob::transfer, true, false, true) &&
                   shouldReturnWorkerCargo(WorkerJob::idle, true, true, false),
               "released economic jobs recover cargo while idle return behavior remains")) return 1;

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
