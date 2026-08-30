#include "astra/Combat.hpp"
#include "astra/CommandBus.hpp"
#include "astra/GameState.hpp"
#include "astra/Geometry.hpp"
#include "astra/InfluenceMap.hpp"
#include "astra/Information.hpp"
#include "astra/Learning.hpp"
#include "astra/MacroPlanner.hpp"
#include "astra/Scouting.hpp"
#include "astra/Strategy.hpp"
#include "astra/Squads.hpp"
#include "astra/UnitCatalog.hpp"
#include "astra/Workers.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void testGeometry() {
    expect(std::abs(astra::distance({0, 0}, {3, 4}) - 5.0) < 0.001,
           "Euclidean distance");
    expect(astra::moveToward({0, 0}, {100, 0}, 32.0) == astra::Position{32, 0},
           "bounded movement toward a target");
}

void testSnapshots() {
    astra::UnitSnapshot dragoon;
    dragoon.id = 7;
    dragoon.hitPoints = 80;
    dragoon.maxHitPoints = 100;
    dragoon.shields = 40;
    dragoon.maxShields = 80;
    dragoon.groundWeapon = {.damage = 20, .targetsGround = true};

    astra::UnitSnapshot enemy;
    enemy.id = 9;

    expect(std::abs(dragoon.healthFraction() - (120.0 / 180.0)) < 0.001,
           "combined shield and hit point fraction");
    expect(dragoon.canAttack(enemy), "ground target compatibility");

    astra::GameState state;
    state.self.units.push_back(dragoon);
    state.enemy.units.push_back(enemy);
    expect(state.findUnit(9).has_value(), "unit lookup across both players");
    expect(!state.findUnit(42).has_value(), "missing unit lookup");
}

astra::UnitSnapshot unit(
    const astra::UnitId id,
    const astra::UnitKind kind,
    const bool ours,
    const astra::Position position = {128, 128}) {
    astra::UnitSnapshot result;
    result.id = id;
    result.kind = kind;
    result.ours = ours;
    result.position = position;
    result.lastPosition = position;
    result.visible = true;
    result.completed = true;
    result.hitPoints = 100;
    result.maxHitPoints = 100;
    result.topSpeed = 4.0;
    return result;
}

void testCatalog() {
    expect(astra::unitStats(astra::UnitKind::probe).minerals == 50,
           "Probe catalog cost");
    expect(astra::unitStats(astra::UnitKind::overlord).name == "Overlord",
           "catalog enum and table remain aligned");
    expect(astra::isCombatUnit(astra::UnitKind::dragoon), "Dragoon combat classification");
    expect(!astra::isCombatUnit(astra::UnitKind::pylon), "Pylon combat classification");
    expect(!astra::isCombatUnit(astra::UnitKind::observer),
           "Observer remains support rather than attack army");
    const auto tribunalRequirements = astra::unitPrerequisites(
        astra::UnitKind::arbiterTribunal);
    expect(std::ranges::find(tribunalRequirements, astra::UnitKind::stargate) !=
               tribunalRequirements.end() &&
               std::ranges::find(tribunalRequirements, astra::UnitKind::templarArchives) !=
                   tribunalRequirements.end(),
           "Arbiter Tribunal requires both branches of its tech tree");
    const auto batteryRequirements = astra::unitPrerequisites(
        astra::UnitKind::shieldBattery);
    expect(batteryRequirements.size() == 1 &&
               batteryRequirements.front() == astra::UnitKind::gateway,
           "Shield Battery follows the actual Gateway prerequisite");
}

void testOpponentInferenceAndStrategy() {
    astra::GameState state;
    state.frame = 3 * 60 * 24;
    state.mapWidthPixels = 4096;
    state.mapHeightPixels = 4096;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.id = 2;
    state.enemy.race = astra::Race::zerg;
    auto nexus = unit(1, astra::UnitKind::nexus, true, {256, 256});
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);
    for (int i = 0; i < 8; ++i) {
        auto zergling = unit(100 + i, astra::UnitKind::zergling, false,
                            {300 + i * 4, 300});
        zergling.role = astra::UnitRole::groundArmy;
        zergling.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                                .targetsGround = true};
        state.enemy.units.push_back(zergling);
    }

    astra::OpponentModel model;
    model.update(state);
    expect(model.mostLikelyPlan() == astra::EnemyPlan::fastRush,
           "early zerglings classify as fast rush");
    expect(model.assessment().immediateGround > 0.3,
           "rush produces immediate-ground warning");

    astra::StrategyEngine strategy;
    const auto plan = strategy.plan(state, model.assessment());
    expect(plan.posture == astra::Posture::defend, "PvZ rush switches to defense");
    const auto emergencyZealots = std::ranges::find_if(
        plan.goals,
        [](const astra::ProductionGoal& goal) {
            return goal.target == astra::UnitKind::zealot && goal.blocking;
        });
    expect(emergencyZealots != plan.goals.end(), "rush plan contains blocking zealots");
}

void testSupplyPlanning() {
    astra::GameState state;
    state.self.id = 1;
    state.self.race = astra::Race::protoss;
    state.enemy.race = astra::Race::terran;
    state.self.supplyUsed = 16;
    state.self.supplyTotal = 18;
    auto nexus = unit(1, astra::UnitKind::nexus, true);
    nexus.role = astra::UnitRole::resourceDepot;
    state.self.units.push_back(nexus);

    astra::StrategyEngine strategy;
    const auto opening = strategy.plan(state, {});
    const auto pylonGoal = std::ranges::find(
        opening.goals, astra::UnitKind::pylon, &astra::ProductionGoal::target);
    expect(pylonGoal != opening.goals.end() && pylonGoal->desiredCount == 1,
           "opening supply logic requests one pylon rather than overbuilding two");

    auto pendingPylon = unit(2, astra::UnitKind::pylon, true);
    pendingPylon.completed = false;
    pendingPylon.buildProgress = 20;
    state.self.units.push_back(pendingPylon);
    const auto constructing = strategy.plan(state, {});
    const auto constructingGoal = std::ranges::find(
        constructing.goals, astra::UnitKind::pylon, &astra::ProductionGoal::target);
    expect(constructingGoal != constructing.goals.end() &&
               constructingGoal->desiredCount == 1,
           "pending pylon supply prevents a duplicate construction order");
}

void testMacroReservations() {
    astra::GameState state;
    state.self.minerals = 200;
    astra::StrategicPlan plan;
    plan.goals = {
        {astra::GoalKind::build, astra::UnitKind::pylon, 1, 100, true, "supply"},
        {astra::GoalKind::build, astra::UnitKind::gateway, 1, 90, false, "production"},
    };
    astra::ResourceLedger ledger{state.self.minerals, 0};
    astra::MacroPlanner planner;
    const auto actions = planner.reconcile(state, plan, ledger);
    expect(actions.size() == 1, "only affordable macro goal is emitted");
    expect(actions.front().target == astra::UnitKind::pylon && actions.front().reserved,
           "higher-priority pylon reserves first");
    expect(ledger.freeMinerals() == 100, "resource reservation is explicit");

    state.self.units.push_back(unit(1, astra::UnitKind::pylon, true));
    astra::StrategicPlan emergency;
    emergency.goals = {
        {astra::GoalKind::build, astra::UnitKind::gateway, 1, 100, true, "emergency"},
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 50, false, "worker"},
    };
    astra::ResourceLedger poor{100, 0};
    const auto waiting = planner.reconcile(state, emergency, poor);
    expect(waiting.size() == 1 && !waiting.front().reserved,
           "unaffordable blocking goal prevents lower-priority spending");
    expect(poor.freeMinerals() == 100, "blocking reservation preserves current bank");

    astra::GameState queuedState;
    queuedState.self.minerals = 50;
    queuedState.self.units.push_back(unit(2, astra::UnitKind::nexus, true));
    queuedState.self.queuedUnits.push_back(astra::UnitKind::probe);
    astra::StrategicPlan queuedPlan;
    queuedPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::probe, 1, 80, false, "worker"},
    };
    astra::ResourceLedger queuedLedger{50, 0};
    expect(planner.reconcile(queuedState, queuedPlan, queuedLedger).empty(),
           "queued production counts toward macro targets");

    astra::GameState duplicateState;
    duplicateState.self.minerals = 800;
    duplicateState.self.units.push_back(unit(3, astra::UnitKind::nexus, true));
    astra::StrategicPlan duplicatePlan;
    duplicatePlan.goals = {
        {astra::GoalKind::expand, astra::UnitKind::nexus, 2, 90, false, "expand"},
        {astra::GoalKind::expand, astra::UnitKind::nexus, 2, 70, false, "economic style"},
    };
    astra::ResourceLedger duplicateLedger{800, 0};
    const auto expansions = planner.reconcile(duplicateState, duplicatePlan, duplicateLedger);
    expect(expansions.size() == 1 && expansions.front().target == astra::UnitKind::nexus,
           "overlapping strategic goals reserve only one missing structure");

    astra::GameState techState;
    techState.self.minerals = 100;
    astra::StrategicPlan techPlan;
    techPlan.goals = {
        {astra::GoalKind::train, astra::UnitKind::dragoon, 1, 90, false, "tech unit"},
    };
    astra::ResourceLedger techLedger{100, 0};
    const auto techActions = planner.reconcile(techState, techPlan, techLedger);
    expect(techActions.size() == 1 && techActions.front().target == astra::UnitKind::pylon,
           "unreachable unit goals build the next missing prerequisite first");

    astra::GameState compositionState;
    compositionState.self.minerals = 125;
    compositionState.self.gas = 50;
    compositionState.self.supplyTotal = 20;
    compositionState.self.units = {
        unit(4, astra::UnitKind::gateway, true),
        unit(5, astra::UnitKind::cyberneticsCore, true),
    };
    astra::StrategicPlan compositionPlan;
    compositionPlan.composition = {{astra::UnitKind::dragoon, 1.0}};
    astra::ResourceLedger compositionLedger{125, 50};
    const auto compositionActions = planner.reconcile(
        compositionState, compositionPlan, compositionLedger);
    expect(compositionActions.size() == 1 &&
               compositionActions.front().target == astra::UnitKind::dragoon,
           "remaining resources continuously reinforce the planned composition");
}

void testOpponentLearning() {
    astra::OpponentHistory history;
    history.parse(
        "Bot,Map,standard,8,2\n"
        "Bot,Map,aggressive,2,8\n"
        "Bot,Map,economic,3,7\n"
        "Bot,Map,deceptive,1,9\n");
    expect(history.choose("Bot", "Map", 7) == astra::OpeningStyle::standard,
           "UCB learning exploits clearly successful opening");
    history.record("Bot", "Map", astra::OpeningStyle::standard, true);
    const auto encoded = history.serialize();
    astra::OpponentHistory restored;
    restored.parse(encoded);
    expect(restored.lookup("Bot", "Map", astra::OpeningStyle::standard).wins == 9,
           "opponent history round-trips through tournament CSV");

    astra::OpponentHistory fresh;
    const auto exploration = fresh.choose("NewBot", "Map", 3);
    expect(exploration != astra::OpeningStyle::count,
           "fresh opponent selects a valid deterministic exploration arm");
}

void testInfluenceAndCombat() {
    astra::GameState state;
    state.mapWidthPixels = 1024;
    state.mapHeightPixels = 1024;
    auto enemy = unit(20, astra::UnitKind::hydralisk, false, {512, 512});
    enemy.role = astra::UnitRole::groundArmy;
    enemy.groundWeapon = {.damage = 10, .cooldown = 15, .maxRange = 128,
                          .targetsGround = true};
    state.enemy.units.push_back(enemy);
    astra::InfluenceMap influence;
    influence.update(state);
    expect(influence.at({512, 512}).groundThreat > 0.0F,
           "enemy weapon contributes local ground threat");

    auto staleState = state;
    staleState.frame = 24 * 60;
    staleState.enemy.units.front().visible = false;
    staleState.enemy.units.front().lastSeen = 0;
    influence.update(staleState);
    expect(influence.at({512, 512}).groundThreat < 0.01F,
           "stale mobile enemies decay out of the fog-of-war threat field");
    auto cannon = staleState.enemy.units.front();
    cannon.kind = astra::UnitKind::photonCannon;
    cannon.role = astra::UnitRole::staticDefense;
    staleState.enemy.units = {cannon};
    influence.update(staleState);
    expect(influence.at({512, 512}).groundThreat > 0.0F,
           "remembered static defenses persist until their tile is cleared");

    std::vector<astra::UnitSnapshot> friendly;
    for (int i = 0; i < 4; ++i) {
        auto dragoon = unit(30 + i, astra::UnitKind::dragoon, true, {400, 400 + i * 8});
        dragoon.role = astra::UnitRole::groundArmy;
        dragoon.shields = 80;
        dragoon.maxShields = 80;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    astra::CombatEvaluator evaluator;
    const auto estimate = evaluator.evaluate(friendly, state.enemy.units, 1.1, 0.1);
    expect(estimate.decision == astra::FightDecision::engage,
           "overwhelming dragoon force elects to engage");
    expect(evaluator.selectTarget(friendly.front(), state.enemy.units) != nullptr,
           "combat target selection finds compatible target");

    auto wounded = enemy;
    wounded.id = 21;
    wounded.hitPoints = 10;
    const std::vector<astra::UnitSnapshot> targetChoices{wounded, enemy};
    const astra::TargetAllocation lethalVolley[]{
        {wounded.id, 20},
    };
    expect(evaluator.selectTarget(friendly.front(), targetChoices, lethalVolley)->id == enemy.id,
           "focus fire redirects once a target has lethal committed damage");

    auto kiter = friendly.front();
    kiter.position = {200, 200};
    kiter.weaponCooldown = 10;
    auto melee = unit(50, astra::UnitKind::zergling, false, {250, 200});
    melee.role = astra::UnitRole::groundArmy;
    melee.groundWeapon = {.damage = 5, .cooldown = 8, .maxRange = 32,
                          .targetsGround = true};
    astra::CombatEstimate kiteEstimate;
    kiteEstimate.decision = astra::FightDecision::kite;
    astra::InfluenceMap emptyInfluence;
    astra::TacticalController tactics;
    const std::vector<astra::UnitSnapshot> kitingForce{kiter};
    const std::vector<astra::UnitSnapshot> meleeForce{melee};
    const auto kiteOrders = tactics.control(
        kitingForce, meleeForce, kiteEstimate, {900, 900}, {100, 200}, emptyInfluence);
    expect(kiteOrders.size() == 1 && kiteOrders.front().type == astra::CommandType::move &&
               kiteOrders.front().targetPosition.x < kiter.position.x,
           "ranged cooldown micro steps directly away from a nearby melee threat");

    auto templar = unit(60, astra::UnitKind::highTemplar, true, {600, 500});
    templar.role = astra::UnitRole::spellcaster;
    const std::vector<astra::UnitSnapshot> casters{templar};
    const auto casterOrders = tactics.control(
        casters, meleeForce, estimate, {900, 900}, {100, 100}, emptyInfluence, {400, 400});
    expect(casterOrders.size() == 1 && casterOrders.front().source == "spellcaster-screen",
           "high-value spellcasters stay behind the formation screen");
}

void testCommandArbitration() {
    astra::CommandBus bus;
    bus.beginFrame(100, 2);
    bus.submit({7, astra::CommandType::move, -1, {400, 400},
                astra::UnitKind::unknown, 20, 0, "patrol"});
    bus.submit({7, astra::CommandType::attackUnit, 9, {-1, -1},
                astra::UnitKind::unknown, 80, 0, "combat"});
    auto selected = bus.finalize();
    expect(selected.size() == 1 && selected.front().type == astra::CommandType::attackUnit,
           "higher-priority command wins per-unit arbitration");
    bus.markIssued(selected.front());

    bus.beginFrame(101, 2);
    bus.submit(selected.front());
    expect(bus.finalize().empty(), "latency-window duplicate is suppressed");

    bus.clear();
    for (int cycle = 0; cycle < 2; ++cycle) {
        bus.beginFrame(200 + cycle, 0);
        for (int actor = 1; actor <= 4; ++actor) {
            bus.submit({actor, astra::CommandType::move, -1, {actor * 32, 100},
                        astra::UnitKind::unknown, 50, 0, "budget"});
        }
        const auto budgeted = bus.finalize(2);
        expect(budgeted.size() == 2, "combat command budget is enforced");
        if (cycle == 0) {
            expect(budgeted.front().actor == 1 && budgeted.back().actor == 2,
                   "first command-budget slice is deterministic");
        } else {
            expect(budgeted.front().actor == 3 && budgeted.back().actor == 4,
                   "equal-priority commands rotate fairly across ticks");
        }
    }
}

void testWorkersAndScouts() {
    astra::GameState state;
    state.frame = 5000;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 4000,
                           true, false, 8, 1});
    state.bases.push_back({2, {1700, 1700}, {1680, 1700}, 8000, 5000, -1, 0, true, false});
    auto probe = unit(5, astra::UnitKind::probe, true, {260, 260});
    probe.role = astra::UnitRole::worker;
    state.self.units.push_back(probe);
    auto observer = unit(6, astra::UnitKind::observer, true, {300, 300});
    observer.flying = true;
    observer.role = astra::UnitRole::detector;
    state.self.units.push_back(observer);
    state.self.units.push_back(unit(7, astra::UnitKind::assimilator, true, {320, 256}));

    astra::InfluenceMap influence;
    influence.update(state);
    astra::StrategicPlan plan;
    plan.desiredGasWorkers = 1;
    astra::WorkerManager workers;
    const auto assignments = workers.assign(state, plan, influence);
    expect(assignments.size() == 1 && assignments.front().job == astra::WorkerJob::gas,
           "gas policy assigns requested worker count");

    const auto scoutState = state;
    state.bases[1].ownerId = 1;
    state.bases[1].mineralPatches = 8;
    for (int i = 0; i < 16; ++i) {
        auto extra = unit(20 + i, astra::UnitKind::probe, true, {260 + i, 270});
        extra.role = astra::UnitRole::worker;
        state.self.units.push_back(extra);
    }
    plan.desiredGasWorkers = 0;
    const auto balanced = workers.assign(state, plan, influence);
    expect(std::ranges::any_of(balanced, [](const astra::WorkerAssignment& assignment) {
               return assignment.job == astra::WorkerJob::transfer && assignment.baseId == 2;
           }),
           "oversaturated mineral lines transfer workers to an owned expansion");

    const astra::UnitId scouts[]{6};
    astra::ScoutManager scouting;
    const auto orders = scouting.assign(scoutState, scouts, influence);
    expect(orders.size() == 1 && orders.front().target == astra::Position{1700, 1700},
           "scout prioritizes stale unexplored start location");
}

void testLocalSquadsAndDetection() {
    astra::GameState state;
    state.mapWidthPixels = 2048;
    state.mapHeightPixels = 2048;
    state.self.id = 1;
    state.enemy.id = 2;
    state.bases.push_back({1, {256, 256}, {300, 260}, 8000, 5000, 1, 0, true, false});

    std::vector<astra::UnitSnapshot> friendly;
    for (int i = 0; i < 6; ++i) {
        auto dragoon = unit(10 + i, astra::UnitKind::dragoon, true,
                            i < 3 ? astra::Position{300 + i * 24, 300}
                                  : astra::Position{1500 + i * 24, 1500});
        dragoon.role = astra::UnitRole::groundArmy;
        dragoon.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                                .targetsGround = true};
        friendly.push_back(dragoon);
    }
    auto lurker = unit(90, astra::UnitKind::lurker, false, {380, 320});
    lurker.role = astra::UnitRole::groundArmy;
    lurker.burrowed = true;
    lurker.detected = false;
    lurker.groundWeapon = {.damage = 20, .cooldown = 37, .maxRange = 192,
                           .targetsGround = true};
    const std::vector<astra::UnitSnapshot> enemy{lurker};

    astra::StrategicPlan plan;
    plan.rallyPoint = {256, 256};
    plan.attackTarget = {1800, 1800};
    astra::SquadPlanner planner;
    auto squads = planner.form(state, friendly, enemy, plan, {256, 256});
    const auto defense = std::ranges::find_if(squads, [](const astra::Squad& squad) {
        return squad.role == astra::SquadRole::baseDefense;
    });
    expect(defense != squads.end() && defense->needsDetection,
           "cloaked base threat creates detection-aware defense squad");

    auto observer = unit(100, astra::UnitKind::observer, true, {200, 200});
    observer.flying = true;
    observer.role = astra::UnitRole::detector;
    state.self.units.push_back(observer);
    astra::InfluenceMap influence;
    influence.update(state);
    const auto escorts = planner.detectorEscorts(state, squads, influence);
    expect(!escorts.empty() && escorts.front().actor == observer.id,
           "observer is assigned to highest-priority detection squad");

    squads = planner.form(state, friendly, {}, plan, {256, 256});
    const auto mainGroups = std::ranges::count_if(squads, [](const astra::Squad& squad) {
        return squad.role == astra::SquadRole::mainArmy;
    });
    expect(mainGroups == 2, "disconnected armies receive independent local decisions");
}

}  // namespace

int main() {
    testGeometry();
    testSnapshots();
    testCatalog();
    testOpponentInferenceAndStrategy();
    testSupplyPlanning();
    testMacroReservations();
    testOpponentLearning();
    testInfluenceAndCombat();
    testCommandArbitration();
    testWorkersAndScouts();
    testLocalSquadsAndDetection();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "All Astra core tests passed\n";
    return EXIT_SUCCESS;
}
