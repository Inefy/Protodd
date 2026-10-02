#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"

#include <algorithm>
#include <iostream>

namespace {
protodd::UnitSnapshot fighter(int id, protodd::UnitKind kind,
                              bool ours, protodd::Position position) {
    using namespace protodd;
    UnitSnapshot unit;
    unit.id = id;
    unit.kind = kind;
    unit.ours = ours;
    unit.position = position;
    unit.completed = unit.visible = unit.detected = unit.powered = true;
    unit.hitPoints = unit.maxHitPoints = 100;
    unit.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                         .targetsGround = true};
    return unit;
}

std::size_t mobileDefenders(const std::vector<protodd::Squad>& squads) {
    using namespace protodd;
    auto count = std::size_t{0};
    for (const auto& squad : squads) {
        if (squad.role != SquadRole::baseDefense) continue;
        count += static_cast<std::size_t>(std::ranges::count_if(squad.units,
            [](const UnitSnapshot& unit) { return !isBuilding(unit.kind); }));
    }
    return count;
}
}

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&failures](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    GameState state;
    state.self.id = 1;
    state.enemy.id = 2;
    state.frame = 1000;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    BaseSnapshot base;
    base.id = 1;
    base.center = {500, 500};
    base.mineralLine = {400, 500};
    base.ownerId = state.self.id;
    state.bases = {base};
    StrategicPlan plan;
    plan.posture = Posture::hold;
    plan.rallyPoint = base.center;
    plan.attackTarget = {1800, 1800};
    SquadPlanner planner;
    std::vector<UnitSnapshot> army;
    for (int i = 0; i < 10; ++i)
        army.push_back(fighter(i, UnitKind::dragoon, true, {500 + i * 8, 500}));
    std::vector<UnitSnapshot> enemies;
    for (int i = 0; i < 3; ++i) {
        auto tank = fighter(100 + i, UnitKind::siegeTank, false, {1100, 480 + i * 24});
        tank.lastSeen = state.frame;
        tank.groundWeapon.maxRange = 384;
        enemies.push_back(tank);
    }
    const auto allocate = [&](const auto& friendly, const auto& hostile) {
        return planner.form(state, friendly, hostile, plan, base.center, nullptr, false, true);
    };
    const auto uncovered = mobileDefenders(allocate(army, enemies));
    check(uncovered == 6, "three healthy tanks request six mobile defenders");

    auto darkTemplars = enemies;
    for (auto& enemy : darkTemplars) {
        enemy.kind = UnitKind::darkTemplar;
        enemy.maxHitPoints = enemy.hitPoints = 80;
        enemy.maxShields = enemy.shields = 40;
        enemy.groundWeapon.maxRange = 15;
        enemy.detected = false;
        enemy.cloaked = true;
    }
    const auto cloakDefense = mobileDefenders(allocate(army, darkTemplars));
    check(cloakDefense == 5, "three healthy Dark Templar request five defenders");
    auto hidden = darkTemplars;
    for (auto& enemy : hidden) {
        enemy.hitPoints = enemy.shields = 0;
    }
    check(mobileDefenders(allocate(army, hidden)) == cloakDefense,
          "unavailable cloaked health cannot shrink the defensive force");
    check(mobileDefenders(planner.form(state, army, hidden, plan, base.center)) == cloakDefense,
          "unknown-health correction remains active in default allocation");
    check(hidden.front().durability() == 0 && !hidden.front().detected,
          "allocation preserves legal health and detection observations");
    auto remoteDt = fighter(300, UnitKind::darkTemplar, true, {1800, 1800});
    remoteDt.cloaked = true;
    auto localDt = remoteDt; localDt.id = 301; localDt.position = base.center;
    const std::vector covertArmy{remoteDt, localDt};
    const auto defending = [&](const auto& squads, int id) {
        return std::ranges::any_of(squads, [id](const Squad& squad) {
            return squad.role == SquadRole::baseDefense &&
                   std::ranges::any_of(squad.units, [id](const UnitSnapshot& member) { return member.id == id; });
        });
    };
    const auto unseenBreach = allocate(covertArmy, darkTemplars);
    check(!defending(unseenBreach, remoteDt.id) && defending(unseenBreach, localDt.id),
          "undetected breach retains nearby DTs without recalling a distant covert raider");
    auto revealedBreach = darkTemplars;
    for (auto& enemy : revealedBreach) enemy.detected = true;
    check(defending(allocate(covertArmy, revealedBreach), remoteDt.id),
          "a detected breach can still recall the DT raid for real defense");
    for (auto& enemy : hidden) enemy.hitPoints = 1;
    check(mobileDefenders(allocate(army, hidden)) == 2,
          "known wounded enemies retain the lower defensive demand");

    auto supported = army;
    for (int i = 0; i < 4; ++i) {
        auto cannon = fighter(200 + i, UnitKind::photonCannon, true, {420, 450 + i * 24});
        cannon.groundWeapon.maxRange = 224;
        supported.push_back(cannon);
    }
    check(mobileDefenders(allocate(supported, enemies)) == uncovered,
          "rear Cannons cannot replace mobile defenders against distant siege");
    check(mobileDefenders(planner.form(state, supported, enemies, plan, base.center)) == 2,
          "unpromoted static-coverage intervention stays off by default");
    auto covered = enemies;
    for (auto& enemy : covered) enemy.position.x = 600;
    check(mobileDefenders(allocate(supported, covered)) == 2,
          "Cannons still reduce mobile demand when all attackers are in range");
    auto partial = enemies;
    partial.front().position.x = 600;
    check(mobileDefenders(allocate(supported, partial)) == 4,
          "four Cannons covering one attacker cannot pay for two uncovered tanks");
    for (auto& enemy : covered) enemy.detected = false;
    check(mobileDefenders(allocate(supported, covered)) == uncovered,
          "static credit requires currently targetable attackers");
    for (auto& cannon : supported)
        if (cannon.kind == UnitKind::photonCannon) cannon.powered = false;
    for (auto& enemy : covered) enemy.detected = true;
    check(mobileDefenders(allocate(supported, covered)) == uncovered,
          "unpowered Cannons provide no defensive credit");
    for (auto& cannon : supported)
        if (cannon.kind == UnitKind::photonCannon) cannon.powered = true;
    plan.posture = Posture::defend;
    check(mobileDefenders(allocate(supported, enemies)) == 6 &&
          mobileDefenders(allocate(supported, partial)) == 4,
          "emergency posture preserves coverage limits with its larger defense margin");
    plan.posture = Posture::hold;
    auto airArmy = army;
    auto airScreen = supported;
    for (auto& member : airArmy) {
        member.airWeapon = member.groundWeapon;
        member.airWeapon.targetsAir = true;
    }
    for (auto& member : airScreen) {
        member.airWeapon = member.groundWeapon;
        member.airWeapon.targetsAir = true;
    }
    auto airThreats = enemies;
    for (auto& enemy : airThreats) {
        enemy.kind = UnitKind::wraith;
        enemy.flying = true;
    }
    check(mobileDefenders(allocate(airScreen, airThreats)) ==
              mobileDefenders(allocate(airArmy, airThreats)),
          "distant air raiders cannot borrow rear Cannon ground-weapon coverage");
    std::ranges::reverse(army);
    std::ranges::reverse(enemies);
    check(mobileDefenders(allocate(army, enemies)) == uncovered,
          "allocation is deterministic across snapshot order");
    return failures == 0 ? 0 : 1;
}
