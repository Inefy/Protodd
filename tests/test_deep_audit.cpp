#include "protodd/Combat.hpp"
#include "protodd/CommandBus.hpp"
#include "protodd/Harassment.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Technology.hpp"
#include "protodd/Scouting.hpp"
#include "protodd/Workers.hpp"
#include "../src/bwapi/TechnologyProducer.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <string_view>

namespace {
using namespace protodd;
int failures{};
void check(bool value, std::string_view message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(int id, UnitKind kind, Position position = {500, 500}, bool ours = true) {
    UnitSnapshot u;
    u.id = id; u.kind = kind; u.position = position; u.ours = ours;
    u.visible = u.completed = u.powered = true;
    u.hitPoints = u.maxHitPoints = 100;
    u.topSpeed = 4.0;
    return u;
}
void navigation() {
    const NavigationGrid blocked{2, 2, 32, {1, 0, 0, 1}};
    check(!blocked.lineWalkable({16, 16}, {48, 48}), "diagonal cannot cut two blocked corners");
    check(!blocked.nextWaypoint({16, 16}, {48, 48}).valid(), "unreachable corner has no direct waypoint");
    const NavigationGrid oneCorner{2, 2, 32, {1, 0, 1, 1}};
    check(!oneCorner.lineWalkable({16, 16}, {48, 48}), "diagonal cannot cut one blocked corner");
    check(oneCorner.findPath({16, 16}, {48, 48}).size() == 3, "legal orthogonal detour is preserved");
    const NavigationGrid open{3, 3, 32, std::vector<std::uint8_t>(9, 1)};
    check(open.lineWalkable({16, 16}, {80, 80}) && open.lineWalkable({80, 80}, {16, 16}),
          "open diagonal remains legal in both directions");
    check(open.lineWalkable({16, 16}, {80, 16}), "open horizontal remains legal");
}
void meleeTargets() {
    auto zealot = unit(1, UnitKind::zealot);
    zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
    auto near = unit(2, UnitKind::marine, {600, 500}, false);
    auto far = unit(3, UnitKind::marine, {850, 500}, false);
    CombatEvaluator evaluator;
    const auto select = [&](UnitSnapshot target, std::span<const TargetAllocation> allocation = {}) {
        const std::array enemies{target, far};
        const auto* chosen = evaluator.selectTarget(zealot, enemies, allocation);
        return chosen == nullptr ? -1 : chosen->id;
    };
    near.invincible = true;
    check(select(near) == far.id, "invincible nearby unit must not hide a legal melee target");
    near.invincible = false; near.loaded = true;
    check(select(near) == far.id, "loaded nearby unit must not hide a legal melee target");
    near.loaded = false; near.incomingDamage = 100;
    check(select(near) == far.id, "lethal incoming projectile must not suppress another melee target");
    near.incomingDamage = 0;
    const std::array allocation{TargetAllocation{near.id, 100}};
    check(select(near, allocation) == far.id, "fully allocated target must not suppress another melee target");
    check(select(near) == near.id, "a legal close enemy still takes precedence over distant pursuit");
}
void threatAndRoutes() {
    GameState state;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    auto dt = unit(10, UnitKind::darkTemplar, {512, 512}, false);
    dt.detected = false; dt.cloaked = true;
    dt.groundWeapon = {40, 30, 0, 15, DamageType::normal, false, true};
    state.enemy.units = {dt};
    InfluenceMap influence;
    influence.update(state);
    const auto full = influence.at(dt.position).groundThreat;
    state.enemy.units[0].hitPoints = 0;
    influence.update(state);
    check(full > 0 && std::abs(influence.at(dt.position).groundThreat - full) < 0.000001F,
          "unknown cloaked health must retain conservative map threat");
    check(state.enemy.units[0].hitPoints == 0 && !state.enemy.units[0].detected,
          "threat estimation cannot rewrite legal observations");
    state.enemy.units[0].detected = true;
    influence.update(state);
    check(influence.at(dt.position).groundThreat == 0, "known zero health must not be inflated");
    state.enemy.units[0] = dt; state.enemy.units[0].loaded = true;
    influence.update(state);
    check(influence.at(dt.position).groundThreat == 0, "loaded cargo cannot project weapon threat");

    auto raider = unit(20, UnitKind::zealot, {100, 512});
    auto cannon = unit(21, UnitKind::photonCannon, {600, 512}, false);
    cannon.groundWeapon = {20, 22, 0, 224, DamageType::normal, false, true};
    state.enemy.units = {cannon};
    check(!harassmentRouteSafe(state, raider, {1100, 512}, false), "powered Cannon blocks harassment route");
    state.enemy.units[0].powered = false;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "unpowered Cannon cannot block route by weapon threat");
    state.enemy.units[0] = unit(22, UnitKind::marine, {600, 512}, false);
    state.enemy.units[0].groundWeapon = cannon.groundWeapon;
    state.enemy.units[0].hallucination = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "hallucinated weapon cannot block harassment route");
    state.enemy.units[0].hallucination = false; state.enemy.units[0].loaded = true;
    check(harassmentRouteSafe(state, raider, {1100, 512}, false), "loaded cargo cannot block harassment route");
}
void stormSafety() {
    auto templar = unit(30, UnitKind::highTemplar);
    templar.role = UnitRole::spellcaster; templar.energy = 100;
    std::vector<UnitSnapshot> enemies;
    for (int i = 0; i < 4; ++i) {
        auto muta = unit(40 + i, UnitKind::mutalisk, {650 + i * 8, 500}, false);
        muta.flying = true; enemies.push_back(muta);
    }
    auto ally = unit(50, UnitKind::carrier, {650, 500}); ally.flying = true;
    const std::array squad{templar};
    CombatEstimate estimate; estimate.decision = FightDecision::engage;
    InfluenceMap influence;
    const auto casts = [&](std::span<const UnitSnapshot> support, std::span<const UnitSnapshot> own) {
        const auto commands = TacticalController{}.control(squad, enemies, estimate, {900, 900},
            {100, 100}, influence, {500, 500}, 3, true, {}, TacticalIntent::battle, support, nullptr, own);
        return std::ranges::any_of(commands, [](const Command& c) {
            return c.technology == TechnologyKind::psionicStorm;
        });
    };
    check(casts({}, {}), "clear enemy cluster still receives Storm");
    check(!casts(std::array{ally}, {}), "Storm must protect an allied unit in another squad");
    check(!casts({}, std::array{ally}), "Storm must protect own units supplied by the live adapter");
    std::vector<UnitSnapshot> workers;
    for (int i = 0; i < 24; ++i) workers.push_back(unit(70 + i, UnitKind::probe, {650, 500}));
    check(!casts({}, workers), "Storm must protect workers outside combat squads");
    ally.loaded = true;
    check(casts(std::array{ally}, std::array{ally}), "loaded allies cannot receive Storm damage");
    auto light = unit(99, UnitKind::zealot, {650, 500});
    check(casts(std::array{light}, std::array{light}), "overlapping spans must not double count friendly fire");
}
void upgrades() {
    GameState state; state.self.supplyTotal = 40;
    state.self.units = {unit(1, UnitKind::nexus), unit(2, UnitKind::pylon),
        unit(3, UnitKind::gateway), unit(4, UnitKind::cyberneticsCore),
        unit(5, UnitKind::citadelOfAdun), unit(6, UnitKind::forge)};
    state.self.technologies = {{TechnologyKind::protossGroundWeapons, 1, false}};
    StrategicPlan plan;
    plan.goals = {{GoalKind::upgrade, UnitKind::unknown, 2, 100, true, "weapons", TechnologyKind::protossGroundWeapons}};
    ResourceLedger ledger{150, 200};
    auto actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::templarArchives &&
          actions[0].reserved && actions[0].action == MacroActionKind::build,
          "level two ground upgrade must unlock its Archives before reserving upgrade cost");
    plan.goals[0].blocking = false; ledger = {150, 200};
    check(MacroPlanner{}.reconcile(state, plan, ledger).empty() && ledger.freeMinerals() == 150,
          "optional upgrade must wait for missing level prerequisite without trapping resources");
    plan.goals[0].blocking = true;
    auto archives = unit(7, UnitKind::templarArchives);
    archives.completed = false; archives.buildProgress = 0;
    state.self.units.push_back(archives); ledger = {150, 200};
    check(MacroPlanner{}.reconcile(state, plan, ledger).empty() && ledger.freeMinerals() == 150,
          "early Archives construction must not reserve an unusable upgrade");
    state.self.units.back().buildProgress = 90; ledger = {150, 200};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && !actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundWeapons,
          "near-complete Archives can reserve but cannot execute level two");
    state.self.units.back().completed = true; ledger = {150, 200};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundWeapons, "completed Archives permits level two");
    state.self.technologies = {{TechnologyKind::protossGroundWeapons, 0, true}};
    plan.goals = {{GoalKind::upgrade, UnitKind::unknown, 1, 100, true, "armor", TechnologyKind::protossGroundArmor},
                  {GoalKind::train, UnitKind::probe, 1, 90, false, "workers"}};
    ledger = {100, 100}; actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].target == UnitKind::probe,
          "existing busy-Forge protection keeps worker funding available");
    state.self.technologies.clear(); plan.goals.resize(1);
    auto secondForge = unit(8, UnitKind::forge);
    secondForge.completed = false; secondForge.buildProgress = 0;
    state.self.units.push_back(secondForge); ledger = {100, 100};
    actions = MacroPlanner{}.reconcile(state, plan, ledger);
    check(actions.size() == 1 && actions[0].reserved && actions[0].executable &&
          actions[0].technology == TechnologyKind::protossGroundArmor,
          "a new unfinished Forge cannot block the existing completed Forge");
}
void unavailableWorkers() {
    GameState state; state.self.id = 1; state.enemy.id = 2;
    state.bases = {{1, {500, 500}, {450, 500}, 8000, 5000, 1}};
    state.self.units = {unit(1, UnitKind::probe), unit(2, UnitKind::probe),
                       unit(3, UnitKind::probe), unit(4, UnitKind::probe),
                       unit(5, UnitKind::pylon)};
    state.self.units[0].loaded = true;
    state.self.units[1].disabled = true;
    state.self.units[2].hallucination = true;
    InfluenceMap influence; influence.update(state);
    const auto assignments = WorkerManager{}.assign(state, {}, influence);
    check(assignments.size() == 1 && assignments[0].worker == 4,
          "loaded, disabled and hallucinated workers cannot fill economic assignments");
    check(selectOpeningWorkerScout(state, {}, {}) == 4,
          "opening scout selection must skip unavailable workers");
    state.self.units[0].loaded = false;
    check(selectOpeningWorkerScout(state, {}, {}) == 1, "unloaded worker becomes eligible again");
}
void commandIdentity() {
    CommandBus bus; bus.beginFrame(100, 3);
    Command spell{1, CommandType::useTech, -1, {500, 500}, UnitKind::unknown,
                  98, 0, "spell", TechnologyKind::stasisField};
    bus.markIssued(spell); bus.beginFrame(101, 3);
    bus.submit(spell); check(bus.finalize().empty(), "identical spell remains suppressed within latency window");
    bus.beginFrame(101, 3); spell.technology = TechnologyKind::recall; bus.submit(spell);
    check(bus.finalize().size() == 1, "different technology is not the same command");
}
void legalTechnologyProducer() {
    struct Producer {
        int id; bool alive; bool completed; bool powered; bool researching; bool upgrading;
        bool exists() const { return alive; }
        bool isCompleted() const { return completed; }
        int getID() const { return id; }
        bool legal() const { return powered && !researching && !upgrading; }
    };
    Producer first{1, true, true, false, false, false};
    Producer second{2, true, true, true, false, false};
    const std::array producers{&first, &second};
    const auto choose = [&] {
        return bwapi::technologyProducer(producers, [](const Producer* p) { return p->legal(); });
    };
    check(choose() == &second, "unpowered lower-ID building cannot hide commandable producer");
    first.powered = true; first.researching = true;
    check(choose() == &second, "researching building cannot hide idle upgrade producer");
    first.researching = false; first.upgrading = true;
    check(choose() == &second, "upgrading building cannot hide idle research producer");
    first.upgrading = false;
    check(choose() == &first, "legal producer choice preserves stable ID ordering");
    first.alive = false; second.completed = false;
    check(choose() == nullptr, "dead and unfinished producers cannot receive commands");
}
void ammunitionAndTargetDomain() {
    auto corsair = unit(1, UnitKind::corsair); corsair.flying = true;
    corsair.airWeapon = {5, 8, 0, 160, DamageType::explosive, true, false};
    auto reaver = unit(2, UnitKind::reaver, {600, 500}, false);
    reaver.ammo = 0;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    const std::array enemy{reaver};
    CombatEvaluator evaluator;
    check(evaluator.evaluate(std::array{corsair}, enemy, 1.2, 0.0, false).enemyPower == 0.0,
          "empty Reaver cannot acquire anti-air threat merely by running out of Scarabs");
    auto zealot = unit(3, UnitKind::zealot);
    zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true, 2};
    check(evaluator.evaluate(std::array{zealot}, enemy, 1.2, 0.0, false).enemyPower > 0.0,
          "empty Reaver retains existing potential threat against compatible ground units");
}
}
int main() {
    navigation(); meleeTargets(); threatAndRoutes(); stormSafety(); upgrades(); commandIdentity(); unavailableWorkers();
    legalTechnologyProducer();
    ammunitionAndTargetDomain();
    if (!failures) std::cout << "Deep audit regression scenarios passed\n";
    return failures ? 1 : 0;
}
