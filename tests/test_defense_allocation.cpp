#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/ConstructionAnchor.hpp"

#include <algorithm>
#include <iostream>
#include <set>
#include <unordered_set>

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

namespace {
using namespace protodd;

UnitSnapshot rankFighter(const int id, const UnitKind kind, const Position position,
                         const bool ours = true) {
    UnitSnapshot unit;
    unit.id = id; unit.kind = kind; unit.position = position; unit.ours = ours;
    unit.completed = unit.visible = unit.detected = unit.powered = true;
    unit.hitPoints = unit.maxHitPoints = 100;
    unit.groundWeapon = {.damage = 20, .cooldown = 30, .maxRange = 192,
                         .targetsGround = true};
    unit.topSpeed = 3.0;
    return unit;
}

std::set<UnitId> responseMembers(const std::vector<Squad>& squads) {
    std::set<UnitId> ids;
    for (const auto& squad : squads) {
        if (squad.role != SquadRole::baseDefense) continue;
        for (const auto& unit : squad.units)
            if (!isBuilding(unit.kind)) ids.insert(unit.id);
    }
    return ids;
}

void checkOpeningScreenStaging(int& failures) {
    const auto check = [&](const bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    GameState state;
    state.frame = 3860;
    state.self.id = 1; state.enemy.id = 2;
    state.self.race = state.enemy.race = Race::protoss;
    state.mapWidthPixels = state.mapHeightPixels = 3072;
    state.self.supplyUsed = 34; state.self.supplyTotal = 50;
    BaseSnapshot base;
    base.id = 1; base.ownerId = 1; base.startLocation = true;
    base.center = {1056, 272}; base.mineralLine = {896, 224};
    base.mineralPatches = 8; base.mineralsRemaining = 8000;
    base.defense.anchor = {1724, 404}; base.defense.entrance = {1788, 524};
    base.defense.width = 256;
    state.bases = {base};
    auto nexus = rankFighter(1, UnitKind::nexus, base.center);
    nexus.role = UnitRole::resourceDepot; nexus.groundWeapon = {};
    auto gateway = rankFighter(2, UnitKind::gateway, {1088, 400});
    gateway.groundWeapon = {};
    auto zealot = rankFighter(179, UnitKind::zealot, {1048, 448});
    zealot.shields = zealot.maxShields = 60; zealot.topSpeed = 4.0;
    zealot.groundWeapon = {.damage = 8, .cooldown = 22,
                           .maxRange = 15, .targetsGround = true};
    zealot.groundWeapon.hits = 2;
    zealot.dimensionLeft = zealot.dimensionRight = 11;
    zealot.dimensionUp = 5; zealot.dimensionDown = 13;
    state.self.units = {nexus, gateway, zealot};
    for (int index = 0; index < 14; ++index) {
        auto probe = rankFighter(20 + index, UnitKind::probe, base.mineralLine);
        probe.role = UnitRole::worker;
        state.self.units.push_back(probe);
    }
    const auto plan = StrategyEngine{}.plan(state, {});
    check((plan.posture == Posture::hold || plan.posture == Posture::defend) &&
          !plan.expansionTarget.valid(), "opening fixture exercises a holding one-base rally");
    check(plan.rallyPoint.valid() && distance(plan.rallyPoint, base.center) <= 160,
          "a lone opening defender must not rally hundreds of pixels ahead of the economy");
    check(plan.constructionRallyPoint() == base.defense.anchor &&
          constructionBuilderAnchor(UnitKind::photonCannon, base.center,
                                    plan.constructionRallyPoint(), plan.expansionTarget) == base.center,
          "temporary army staging preserves the planned front while distant static placement screens at the base");
    auto compactState = state;
    compactState.bases.front().defense.anchor = {base.center.x + 128, base.center.y};
    const auto compactPlan = StrategyEngine{}.plan(compactState, {});
    check(constructionBuilderAnchor(UnitKind::photonCannon, base.center,
                                    compactPlan.constructionRallyPoint(), compactPlan.expansionTarget) ==
              compactState.bases.front().defense.anchor,
          "a supported compact construction front survives temporary army staging");
    for (const auto homeBuilding : {UnitKind::pylon, UnitKind::gateway, UnitKind::forge})
        check(constructionBuilderAnchor(homeBuilding, base.center, plan.constructionRallyPoint(),
                                        plan.expansionTarget) == base.center,
              "separating staging preserves home builder selection");
    check(constructionBuilderAnchor(UnitKind::nexus, base.center, plan.constructionRallyPoint(),
                                    {2200, 2000}) == Position{2200, 2000},
          "explicit expansion construction retains its own destination");
    const auto quietArea = SquadPlanner::defensiveArea(state, base.defense.anchor);
    check(distance(quietArea.center, base.center) <= 160 && !quietArea.front.valid(),
          "quiet squad geometry must not restore an unsupported distant terrain anchor");
    auto enemy = zealot;
    enemy.id = 175; enemy.ours = false; enemy.position = {1788, 524};
    enemy.lastSeen = state.frame;
    state.enemy.units = {enemy};
    StrategicPlan holding;
    holding.posture = Posture::defend;
    holding.rallyPoint = moveToward(base.center, base.defense.anchor, 144.0);
    holding.attackTarget = enemy.position;
    const std::vector<UnitSnapshot> army{zealot};
    const auto squads = SquadPlanner{}.form(state, army, state.enemy.units, holding, base.center);
    const auto defense = std::ranges::find(squads, SquadRole::baseDefense, &Squad::role);
    check(defense != squads.end() && distance(defense->defense.center, base.center) <= 160 &&
          !defense->defense.front.valid() && defense->defense.contains(base.center),
          "first-contact squad formation must preserve a near-economy unsupported screen");
    auto second = zealot;
    second.id = 190; second.position = {1088, 448};
    auto paired = state;
    paired.self.units.push_back(second);
    check(!openingScreenNeedsCover(paired, base, paired.self.units),
          "two ready local defenders can take terrain against one observed attacker");
    auto anotherEnemy = enemy;
    anotherEnemy.id = 176; anotherEnemy.position.y += 32;
    paired.enemy.units.push_back(anotherEnemy);
    check(openingScreenNeedsCover(paired, base, paired.self.units),
          "a merely equal opening force stages near the economy instead of advancing unsupported");
    auto fogContact = paired;
    for (auto& contact : fogContact.enemy.units) { contact.visible = contact.detected = false; }
    check(openingScreenNeedsCover(fogContact, base, fogContact.self.units),
          "recently hidden attackers do not release and recall an equal opening force on visibility changes");
    for (auto& contact : fogContact.enemy.units) contact.locationConfidence = 0.05;
    check(!openingScreenNeedsCover(fogContact, base, fogContact.self.units),
          "expired attacker locations do not indefinitely pin a ready opening force");
    auto third = zealot;
    third.id = 191; third.position = {1120, 448};
    paired.self.units.push_back(third);
    check(!openingScreenNeedsCover(paired, base, paired.self.units),
          "a ready opening force with a local lead retains terrain formation");
    auto remoteFront = paired;
    remoteFront.bases.front().defense.anchor = {2048, 404};
    remoteFront.bases.front().defense.entrance = {2112, 524};
    for (auto& fighter : remoteFront.self.units)
        if (fighter.kind == UnitKind::zealot) fighter.position = remoteFront.bases.front().defense.anchor;
    remoteFront.enemy.units.clear();
    check(!openingScreenNeedsCover(remoteFront, remoteFront.bases.front(), remoteFront.self.units),
          "arriving at a distant terrain front does not make a ready force cease being local support");
    auto airContact = paired;
    auto mutalisk = enemy;
    mutalisk.kind = UnitKind::mutalisk; mutalisk.flying = true;
    airContact.enemy.units = {mutalisk};
    check(openingScreenNeedsCover(airContact, base, airContact.self.units),
          "ground-only fighters cannot release a screen against an observed air attacker");
    for (int unavailable = 0; unavailable < 7; ++unavailable) {
        auto unavailablePair = state;
        auto escort = second;
        switch (unavailable) {
            case 0: escort.completed = false; break;
            case 1: escort.disabled = true; break;
            case 2: escort.loaded = true; break;
            case 3: escort.hallucination = true; break;
            case 4: escort.recharging = true; break;
            case 5: escort.hitPoints = 20; escort.shields = 0; break;
            case 6: escort.kind = UnitKind::reaver; escort.ammo = 0; break;
        }
        unavailablePair.self.units.push_back(escort);
        check(openingScreenNeedsCover(unavailablePair, base, unavailablePair.self.units),
              "an unavailable, wounded or empty-ammo escort cannot release the opening screen");
    }
    auto distantPair = state;
    second.position = {2600, 2400};
    distantPair.self.units.push_back(second);
    check(openingScreenNeedsCover(distantPair, base, distantPair.self.units),
          "a distant raid is not local opening support");
    auto supported = state;
    auto cannon = rankFighter(200, UnitKind::photonCannon, base.defense.anchor);
    cannon.groundWeapon.maxRange = 224; cannon.topSpeed = 0;
    supported.self.units.push_back(cannon);
    check(!openingScreenNeedsCover(supported, base, supported.self.units),
          "completed forward Cannon coverage retains terrain assembly");
    for (int unavailable = 0; unavailable < 5; ++unavailable) {
        auto unavailableSupport = supported;
        auto& tower = unavailableSupport.self.units.back();
        switch (unavailable) {
            case 0: tower.completed = false; break;
            case 1: tower.powered = false; break;
            case 2: tower.disabled = true; break;
            case 3: tower.loaded = true; break;
            case 4: tower.hallucination = true; break;
        }
        check(openingScreenNeedsCover(unavailableSupport, base, unavailableSupport.self.units),
              "an unusable Cannon cannot release an unsupported opening screen");
    }
    supported.self.units.back().position = {1168, 128};
    const auto rearArea = SquadPlanner::defensiveArea(supported, base.defense.anchor);
    check(openingScreenNeedsCover(supported, base, supported.self.units) &&
          rearArea.center == supported.self.units.back().position,
          "rear Cannon support keeps the opening screen within actual support instead of a distant choke");
    auto mature = state;
    mature.frame = 10 * 60 * 24;
    check(!openingScreenNeedsCover(mature, base, mature.self.units),
          "opening staging does not replace mature terrain behavior");
    auto expanded = state;
    auto naturalNexus = nexus;
    naturalNexus.id = 201; naturalNexus.position = {2200, 2000};
    expanded.self.units.push_back(naturalNexus);
    check(!openingScreenNeedsCover(expanded, base, expanded.self.units),
          "multi-base defense does not inherit the one-base opening restriction");
    auto closeTerrain = base;
    closeTerrain.defense.anchor = {1280, 320};
    check(!openingScreenNeedsCover(state, closeTerrain, state.self.units),
          "terrain close to the economy remains usable by the first defender");

    // Same roster, objective and retreat; only ammunition readiness changes
    // whether this defense may use the terrain front. Routing must see it.
    auto armed = state;
    auto reaver = zealot;
    reaver.id = 190; reaver.kind = UnitKind::reaver; reaver.ammo = 1;
    reaver.position = {1088, 448}; reaver.topSpeed = 1.8;
    reaver.groundWeapon = {.damage = 100, .cooldown = 60, .maxRange = 256, .targetsGround = true};
    armed.self.units.push_back(reaver);
    const std::vector<UnitSnapshot> armedArmy{zealot, reaver};
    const auto armedSquads = SquadPlanner{}.form(armed, armedArmy, armed.enemy.units, holding, base.center);
    auto empty = armed;
    empty.self.units.back().ammo = 0;
    auto emptyArmy = armedArmy;
    emptyArmy.back().ammo = 0;
    const auto emptySquads = SquadPlanner{}.form(empty, emptyArmy, empty.enemy.units, holding, base.center);
    const auto armedDefense = std::ranges::find(armedSquads, SquadRole::baseDefense, &Squad::role);
    const auto emptyDefense = std::ranges::find(emptySquads, SquadRole::baseDefense, &Squad::role);
    check(armedDefense != armedSquads.end() && emptyDefense != emptySquads.end() &&
          armedDefense->defense.front.valid() && !emptyDefense->defense.front.valid() &&
          armedDefense->signature != emptyDefense->signature &&
          armedDefense->engagementKey == emptyDefense->engagementKey,
          "changed opening defense geometry invalidates routing identity without resetting fight hysteresis");
}

void checkDefenderResponseStability(int& failures) {
    const auto check = [&](const bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    GameState state;
    state.self.id = 1; state.enemy.id = 2; state.frame = 1000;
    state.mapWidthPixels = state.mapHeightPixels = 2048;
    BaseSnapshot base;
    base.id = 1; base.center = {500, 500}; base.mineralLine = {400, 500}; base.ownerId = 1;
    state.bases = {base};
    StrategicPlan plan;
    plan.posture = Posture::hold; plan.rallyPoint = base.center; plan.attackTarget = {1800, 1800};
    std::vector<UnitSnapshot> army, enemies;
    for (int i = 0; i < 10; ++i)
        army.push_back(rankFighter(i, UnitKind::dragoon, {500 + i * 8, 500}));
    for (int i = 0; i < 3; ++i) {
        auto tank = rankFighter(100 + i, UnitKind::siegeTank, {1100, 480 + i * 24}, false);
        tank.groundWeapon.maxRange = 384; tank.lastSeen = state.frame;
        enemies.push_back(tank);
    }
    const auto initialState = state;
    const auto initialArmy = army;
    SquadPlanner planner;
    const auto form = [&] {
        return responseMembers(planner.form(state, army, enemies, plan, base.center, nullptr, false));
    };
    const auto original = form();
    check(original.size() == 6, "fixture commits six capable mobile defenders");
    // The same healthy force and threat: a24px movement crosses nearest-arrival ranking.
    for (auto& unit : army) unit.position.x += original.contains(unit.id) ? -24 : 24;
    state.frame += 24;
    for (auto& enemy : enemies) enemy.lastSeen = state.frame;
    const auto next = form();
    check(next == original, "same healthy defensive response must survive a small arrival-rank crossing");
    std::ranges::reverse(army);
    check(form() == next, "rank crossing is independent of snapshot iteration order");
    const auto disabledId = *next.begin();
    for (auto& unit : army) if (unit.id == disabledId) unit.disabled = true;
    state.frame += 24;
    for (auto& enemy : enemies) enemy.lastSeen = state.frame;
    const auto replacement = form();
    check(replacement.size() == 6 && !replacement.contains(disabledId),
          "disabled defender is immediately replaced without reducing demand");
    // A materially slower response must yield to available faster responders.
    for (auto& unit : army) {
        unit.disabled = false;
        unit.position.x = replacement.contains(unit.id) ? 1600 : 640;
    }
    state.frame += 24;
    for (auto& enemy : enemies) enemy.lastSeen = state.frame;
    const auto urgent = form();
    check(urgent.size() == 6 && std::ranges::any_of(army, [&](const auto& unit) {
              return !replacement.contains(unit.id) && urgent.contains(unit.id);
          }), "materially faster responders are not excluded by a prior assignment");
    planner.reset();
    check(form() == responseMembers(SquadPlanner{}.form(
              state, army, enemies, plan, base.center, nullptr, false)),
          "reset releases any defensive assignment history");

    const auto invalidatedResponse = [&](const auto& invalidate, const char* message) {
        auto localState = initialState;
        auto localArmy = initialArmy;
        auto localEnemies = enemies;
        for (auto& enemy : localEnemies) enemy.lastSeen = localState.frame;
        SquadPlanner localPlanner;
        const auto selected = responseMembers(localPlanner.form(
            localState, localArmy, localEnemies, plan, base.center, nullptr, false));
        for (auto& unit : localArmy) unit.position.x += selected.contains(unit.id) ? -24 : 24;
        localState.frame += 24;
        invalidate(localPlanner, localState, localArmy, selected);
        for (auto& enemy : localEnemies) enemy.lastSeen = localState.frame;
        const auto observed = responseMembers(localPlanner.form(
            localState, localArmy, localEnemies, plan, base.center, nullptr, false));
        const auto fresh = responseMembers(SquadPlanner{}.form(
            localState, localArmy, localEnemies, plan, base.center, nullptr, false));
        check(observed == fresh, message);
    };
    invalidatedResponse([](auto&, auto& localState, auto& localArmy, const auto&) {
        for (auto& unit : localArmy) unit.firstSeen = localState.frame;
    }, "a reused actor lifetime cannot inherit a prior defensive response");
    invalidatedResponse([](auto&, auto& localState, auto&, const auto&) {
        localState.frame += 73;
    }, "a stale defensive response does not override current arrival ranking");
    invalidatedResponse([](auto&, auto& localState, auto&, const auto&) {
        localState.bases.front().id = 2;
    }, "response preference belongs to the current base identity");
    invalidatedResponse([](auto&, auto&, auto& localArmy, const auto& selected) {
        for (auto& unit : localArmy) if (selected.contains(unit.id)) unit.hitPoints = 25;
    }, "wounded defenders lose preference while demand uses current health");
    invalidatedResponse([](auto& localPlanner, auto&, auto&, const auto& selected) {
        for (const auto id : selected) localPlanner.forgetUnit(id);
    }, "forgotten actors release their defensive response preference");
    invalidatedResponse([](auto&, auto& localState, auto&, const auto&) {
        localState.frame = 999;
    }, "a frame rollback clears defensive response history");
}
} // namespace

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
        return planner.form(state, friendly, hostile, plan, base.center, nullptr, false);
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
    check(mobileDefenders(planner.form(state, supported, enemies, plan, base.center)) == uncovered,
          "out-of-range Cannons do not receive defense credit by default");
    auto covered = enemies;
    for (auto& enemy : covered) enemy.position.x = 600;
    check(mobileDefenders(allocate(supported, covered)) == 2,
          "Cannons still reduce mobile demand when all attackers are in range");
    auto partial = enemies;
    partial.front().position.x = 600;
    check(mobileDefenders(allocate(supported, partial)) == 4,
          "four Cannons covering one attacker cannot pay for two uncovered tanks");
    check(mobileDefenders(planner.form(state, supported, partial, plan, base.center)) == 4,
          "default defense allocation credits only the one attacker currently covered");
    auto battery = fighter(250, UnitKind::shieldBattery, true, {430, 500});
    battery.groundWeapon = {};
    battery.airWeapon = {};
    auto batterySupported = army;
    batterySupported.push_back(battery);
    check(mobileDefenders(planner.form(state, batterySupported, enemies, plan, base.center)) ==
              uncovered,
          "Shield Batteries support recharge but cannot claim weapon coverage of attackers");
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

    GameState routeState;
    routeState.self.id = 1;
    routeState.enemy.id = 2;
    routeState.mapWidthPixels = 2048;
    routeState.mapHeightPixels = 1024;
    auto routeBase = base;
    routeBase.center = {800, 512};
    routeBase.mineralLine = {900, 512};
    routeState.bases = {routeBase};
    std::vector<std::uint8_t> terrain(64U * 32U, 1U);
    for (int y = 0; y < 32; ++y) {
        if (y != 1) terrain[static_cast<std::size_t>(y * 64 + 26)] = 0U;
        terrain[static_cast<std::size_t>(y * 64 + 40)] = 0U;
    }
    NavigationGrid routeGrid(64, 32, 32, std::move(terrain));
    auto slowRemote = fighter(201, UnitKind::dragoon, true, {880, 512});
    slowRemote.topSpeed = 2.1;
    auto fastLocal = fighter(202, UnitKind::zealot, true, {400, 512});
    fastLocal.topSpeed = 3.2;
    auto unreachable = fighter(203, UnitKind::zealot, true, {1600, 512});
    unreachable.topSpeed = 3.2;
    const auto remotePath = routeGrid.findPath(slowRemote.position, {656, 512});
    const auto localPath = routeGrid.findPath(fastLocal.position, {656, 512});
    const auto pathLength = [](const NavigationPathResult& path, const Position from) {
        auto length = 0.0;
        auto previous = from;
        for (const auto point : path.points) {
            length += distance(previous, point);
            previous = point;
        }
        return length;
    };
    auto nearbySlow = fighter(204, UnitKind::zealot, true, {720, 512});
    nearbySlow.topSpeed = 0.5;
    check(remotePath.reached() && localPath.reached() &&
              pathLength(localPath, fastLocal.position) / fastLocal.topSpeed <
                  pathLength(remotePath, slowRemote.position) / slowRemote.topSpeed,
          "fixture makes the farther fighter faster by reachable route arrival");
    const std::vector routeArmy{slowRemote, fastLocal, unreachable, nearbySlow};
    std::vector<UnitSnapshot> routeThreats;
    routeThreats.push_back(fighter(400, UnitKind::marine, false, {800, 480}));
    const auto routeDefense = planner.form(
        routeState, routeArmy, routeThreats, plan, routeBase.center, &routeGrid, false);
    const auto routeGuard = std::ranges::find(routeDefense, SquadRole::baseDefense,
                                               &Squad::role);
    check(routeGuard != routeDefense.end() && routeGuard->units.size() == 2 &&
              std::ranges::any_of(routeGuard->units, [&](const UnitSnapshot& member) {
                  return member.id == fastLocal.id;
              }) &&
              std::ranges::none_of(routeGuard->units, [&](const UnitSnapshot& member) {
                  return member.id == slowRemote.id || member.id == unreachable.id;
              }),
          "defense ranks footprint-clear path arrival over straight-line proximity and skips a disconnected fighter");

    GameState concurrent;
    // A large distant reserve must not multiply route searches without bound.
    std::vector<std::uint8_t> loadTerrain(160U * 128U, 1U);
    for (int y = 0; y < 128; ++y) loadTerrain[static_cast<std::size_t>(y * 160 + 40)] = 0U;
    NavigationGrid loadGrid(160, 128, 32, std::move(loadTerrain));
    std::vector<UnitSnapshot> loadArmy{
        fighter(800, UnitKind::dragoon, true, {640, 480}),
        fighter(801, UnitKind::dragoon, true, {640, 544})};
    for (int i = 0; i < 80; ++i)
        loadArmy.push_back(fighter(900 + i, UnitKind::dragoon, true,
            {1500 + (i % 10) * 64, 256 + (i / 10) * 64}));
    const auto beforeLoad = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto loadDefense = planner.form(routeState, loadArmy, routeThreats, plan,
        routeBase.center, &loadGrid, false);
    const auto loadSearches = NavigationGrid::diagnosticsForCurrentThread().searches - beforeLoad;
    check(loadSearches <= maximumDefensiveArrivalPathSearches && mobileDefenders(loadDefense) == 2,
          "large defense allocation bounds route searches and keeps confirmed local responders");
    std::ranges::reverse(loadArmy);
    const auto reversedLoadDefense = planner.form(routeState, loadArmy, routeThreats, plan,
        routeBase.center, &loadGrid, false);
    const auto loadIds = [](const auto& squads) {
        std::vector<UnitId> ids;
        for (const auto& squad : squads) if (squad.role == SquadRole::baseDefense)
            for (const auto& member : squad.units) ids.push_back(member.id);
        return ids;
    };
    check(loadIds(loadDefense) == loadIds(reversedLoadDefense),
          "bounded defensive arrival validation is deterministic across snapshot order");
    concurrent.self.id = 1;
    concurrent.enemy.id = 2;
    auto firstBase = base;
    firstBase.id = 1;
    firstBase.center = {500, 500};
    firstBase.mineralLine = {400, 500};
    auto secondBase = base;
    secondBase.id = 2;
    secondBase.center = {1800, 500};
    secondBase.mineralLine = {1900, 500};
    concurrent.bases = {firstBase, secondBase};
    const std::vector concurrentArmy{
        fighter(501, UnitKind::dragoon, true, {600, 500}),
        fighter(502, UnitKind::dragoon, true, {560, 520}),
        fighter(503, UnitKind::dragoon, true, {1700, 500}),
        fighter(504, UnitKind::dragoon, true, {1740, 520}),
    };
    std::vector<UnitSnapshot> concurrentThreats;
    for (int i = 0; i < 2; ++i) {
        const auto offset = i == 0 ? 0 : 1300;
        auto darkTemplar = fighter(600 + i, UnitKind::darkTemplar, false,
                                   {520 + offset, 500});
        darkTemplar.cloaked = true;
        darkTemplar.detected = false;
        concurrentThreats.push_back(darkTemplar);
    }
    auto firstObserver = fighter(701, UnitKind::observer, true, {600, 500});
    firstObserver.flying = true;
    firstObserver.sightRange = 288;
    auto secondObserver = fighter(702, UnitKind::observer, true, {1700, 500});
    secondObserver.flying = true;
    secondObserver.sightRange = 288;
    concurrent.self.units = {firstObserver, secondObserver};
    const auto concurrentDefense = planner.form(
        concurrent, concurrentArmy, concurrentThreats, plan, firstBase.center);
    std::unordered_set<UnitId> assignedDefenders;
    auto defenseSquads = 0;
    auto defenseMembers = 0;
    bool uniqueAssignment = true;
    for (const auto& squad : concurrentDefense) {
        if (squad.role != SquadRole::baseDefense) continue;
        ++defenseSquads;
        for (const auto& member : squad.units) {
            ++defenseMembers;
            uniqueAssignment = assignedDefenders.insert(member.id).second && uniqueAssignment;
        }
    }
    const auto detectorCoverage = planner.allocateDetectors(
        concurrent, concurrentDefense, InfluenceMap{});
    check(defenseSquads == 2 && defenseMembers == 4 && uniqueAssignment,
          "simultaneous base threats receive separate local response groups without double-assigning defenders");
    check(std::ranges::count_if(concurrentDefense, [](const Squad& squad) {
              return squad.role == SquadRole::baseDefense && squad.needsDetection;
          }) == 2,
          "simultaneous cloaked breaches retain independent detection demands");
    check(detectorCoverage.assignments.size() == 2 &&
              detectorCoverage.unmetDetectionDemands == 0 &&
              detectorCoverage.assignments[0].observerId !=
                  detectorCoverage.assignments[1].observerId,
          "detector support is also assigned uniquely across both base-defense commitments");

    std::ranges::reverse(army);
    std::ranges::reverse(enemies);
    check(mobileDefenders(allocate(army, enemies)) == uncovered,
          "allocation is deterministic across snapshot order");
    checkDefenderResponseStability(failures);
    checkOpeningScreenStaging(failures);
    return failures == 0 ? 0 : 1;
}
