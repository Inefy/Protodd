#include "protodd/ConstructionAnchor.hpp"
#include "protodd/Operations.hpp"
#include "protodd/UnitCatalog.hpp"

#include <iostream>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };
    const Position home{2112, 3824}, rally{1424, 3696}, site{992, 3472};
    for (auto kind : {UnitKind::pylon, UnitKind::gateway, UnitKind::forge,
                     UnitKind::cyberneticsCore, UnitKind::roboticsFacility,
                     UnitKind::observatory, UnitKind::roboticsSupportBay,
                     UnitKind::stargate, UnitKind::citadelOfAdun,
                     UnitKind::templarArchives, UnitKind::fleetBeacon,
                     UnitKind::arbiterTribunal})
        check(constructionBuilderAnchor(kind, home, rally, site) == home,
              "home infrastructure selected its builder against the threatened forward rally");
    check(constructionBuilderAnchor(UnitKind::nexus, home, rally, site) == site,
          "Nexus builder selection lost its actual resource site");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, rally, site) == rally,
          "static intercept was relocated to the home tech anchor");
    check(constructionBuilderAnchor(UnitKind::shieldBattery, home, {-1, -1}, site) == home,
          "missing rally did not fall back to home");

    const auto fighter = [](int id, UnitKind kind, Position position) {
        UnitSnapshot unit;
        unit.id = id; unit.kind = kind; unit.position = position;
        unit.completed = unit.visible = true;
        unit.hitPoints = unit.maxHitPoints = kind == UnitKind::marine ? 40 : 100;
        unit.shields = unit.maxShields = kind == UnitKind::marine ? 0 : 80;
        unit.groundWeapon = {.damage = kind == UnitKind::marine ? 6 : 20,
                             .cooldown = kind == UnitKind::marine ? 15 : 30,
                             .maxRange = kind == UnitKind::marine ? 128 : 192,
                             .targetsGround = true};
        return unit;
    };
    GameState state; state.frame = 11000;
    StrategicPlan plan; plan.expansionTarget = site;
    auto marine = fighter(100, UnitKind::marine, {site.x + 200, site.y});
    marine.lastSeen = state.frame;
    state.enemy.units = {marine};
    for (int i = 0; i < 12; ++i)
        state.self.units.push_back(fighter(i, UnitKind::dragoon, home));
    ExpansionCoordinator coordinator;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion && !coordinator.releaseBuilder() && plan.expansionTarget == site,
          "distant army permitted an exposed Nexus or lost its escort mission");
    coordinator.update(plan, state, {site, true, 12});
    check(plan.deferExpansion && coordinator.releaseBuilder(),
          "unsafe walking builder retained an order that could spend after deferral");
    for (int i = 0; i < 4; ++i) state.self.units[i].position = {site.x + 100, site.y};
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "arrived superior local cover did not release construction funding");
    for (auto& unit : state.self.units) unit.loaded = true;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "loaded escorts counted as a site screen");
    auto cannon = fighter(150, UnitKind::photonCannon, site);
    cannon.groundWeapon.maxRange = 224;
    state.self.units.push_back(cannon);
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "completed local static cover was ignored");
    state.self.units.back().powered = false;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "unpowered static cover released Nexus funding");
    state.self.units.pop_back();
    state.enemy.units[0].visible = false;
    coordinator.update(plan, state, {});
    check(plan.deferExpansion, "freshly hidden site threat was forgotten immediately");
    state.frame += 5 * 24 + 1;
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "stale invisible mobile threat held expansion funding forever");
    state.enemy.units[0].visible = true;
    auto nexus = fighter(200, UnitKind::nexus, site); nexus.completed = false;
    state.self.units.push_back(nexus);
    coordinator.update(plan, state, {site, true, 500});
    check(!plan.deferExpansion && !coordinator.releaseBuilder(),
          "site safety gate cancelled an already warping Nexus");
    return failures ? 1 : 0;
}
