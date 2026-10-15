#include "protodd/ConstructionTraffic.hpp"
#include <iostream>

int main() {
    using namespace protodd;
    int failed = 0, checked = 0;
    const auto check = [&](bool value, const char* name) {
        ++checked; if (!value) { ++failed; std::cerr << "FAIL: " << name << '\n'; }
    };
    BuildWaypointProgress progress{{3452,1180},215,10};
    check(!constructionTrafficNeedsClearance(progress,33,{3452,1180}), "progressing route cannot redirect nearby helpers");
    check(constructionTrafficNeedsClearance(progress,34,{3452,1180}), "stalled route admits assistance before48frame retention expires");
    recordBuildWaypointProgress(progress,35,{3452,1000},{3452,1180});
    check(!constructionTrafficNeedsClearance(progress,58,{3452,1180}), "meaningful native progress restarts admission delay");
    check(!constructionTrafficNeedsClearance(progress,59,{3452,1260}), "a different worker waypoint cannot use stale progress");
    check(!constructionTrafficNeedsClearance(progress,34,{3452,1180}), "future progress observation cannot admit assistance");
    check(!constructionTrafficNeedsClearance({},100,{3452,1180}), "missing waypoint progress grants no admission");
    UnitSnapshot builder; builder.id = 900; builder.position = {3456,965};
    builder.dimensionLeft = builder.dimensionRight = builder.dimensionUp = builder.dimensionDown = 11;
    std::vector<UnitSnapshot> units;
    const auto add = [&](int id, UnitKind kind, Position position) {
        UnitSnapshot unit; unit.id = id; unit.kind = kind; unit.position = position;
        unit.ours = unit.visible = unit.completed = true; unit.role = UnitRole::groundArmy;
        unit.hitPoints = unit.maxHitPoints = 100; unit.firstSeen = 2;
        unit.dimensionLeft = kind == UnitKind::zealot ? 11 : 15;
        unit.dimensionRight = kind == UnitKind::zealot ? 11 : 16;
        unit.dimensionUp = kind == UnitKind::zealot ? 5 : 15;
        unit.dimensionDown = kind == UnitKind::zealot ? 13 : 16;
        units.push_back(unit);
    };
    // Legally observed native bodies from the frozen regression; no selector IDs.
    add(17,UnitKind::zealot,{3434,1101}); add(28,UnitKind::zealot,{3444,1070});
    add(39,UnitKind::dragoon,{3387,1052}); add(41,UnitKind::dragoon,{3428,972});
    add(53,UnitKind::dragoon,{3471,1084}); add(64,UnitKind::dragoon,{3419,1047});
    add(75,UnitKind::dragoon,{3433,911});
    check(constructionTrafficCluster(builder,{3452,1180},units) == std::vector<UnitId>{17,28,39,53,64},
          "select all five connected blockers and leave separated upstream bodies");
    std::ranges::reverse(units);
    check(constructionTrafficCluster(builder,{3452,1180},units) == std::vector<UnitId>{17,28,39,53,64},
          "cluster is independent of observation order and fixture IDs");
    check(constructionTrafficCluster(builder,{3656,965},units).empty(), "clear local route needs no helpers");
    check(constructionTrafficCluster(builder,{3452,1600},units).empty(), "long route has no unbounded local clearance");
    auto unit = units.front(); check(constructionTrafficHelper(unit), "healthy ground combat helper");
    unit.loaded = true; check(!constructionTrafficHelper(unit), "loaded actors excluded"); unit.loaded = false;
    unit.recentlyDamaged = true; check(!constructionTrafficHelper(unit), "damaged retreats excluded"); unit.recentlyDamaged = false;
    unit.attackWindup = true; check(!constructionTrafficHelper(unit), "attack windup excluded"); unit.attackWindup = false;
    unit.incomingDamage = 1; check(!constructionTrafficHelper(unit), "imminent projectile excluded"); unit.incomingDamage = 0;
    unit.kind = UnitKind::highTemplar; check(!constructionTrafficHelper(unit), "casters excluded");
    unit.kind = UnitKind::probe; unit.role = UnitRole::worker; check(!constructionTrafficHelper(unit), "workers excluded");
    unit = units.front(); unit.ours = false; check(!constructionTrafficHelper(unit), "foreign units excluded");
    Command proposal; proposal.owner = CommandOwner::combat; proposal.type = CommandType::move;
    proposal.source = "defense-return"; proposal.priority = 90;
    check(constructionTrafficProposal(proposal), "ordinary return proposal eligible");
    proposal.owner = CommandOwner::transport; check(!constructionTrafficProposal(proposal), "foreign authority excluded");
    proposal.owner = CommandOwner::combat; proposal.priority = 100;
    check(!constructionTrafficProposal(proposal), "emergency retreat urgency excluded");
    proposal.priority = 90; proposal.type = CommandType::attackUnit;
    check(!constructionTrafficProposal(proposal), "combat attack proposal preserved");
    proposal.type = CommandType::move; proposal.source = "spell-evade";
    check(!constructionTrafficProposal(proposal), "spell avoidance preserved");
    ConstructionTrafficAssistance assistance;
    assistance.context = {123,900,2,{3680,2320},{3452,1180}};
    assistance.started = assistance.lastProgress = assistance.lastEligible = 10;
    assistance.members = {{17,2,{3636,556},600,false,90},{28,2,{3700,556},600,false,90}};
    check(!assistance.canHold(10,assistance.context), "unacknowledged request grants no worker hold");
    assistance.acknowledge(17,{3636,556},false);
    check(!assistance.acknowledged(), "rejected command grants no assistance credit");
    assistance.acknowledge(17,{3636,556},true);
    check(!assistance.canHold(10,assistance.context), "partial or budget-deferred group grants no hold");
    assistance.acknowledge(28,{3700,556},true);
    check(!assistance.canHold(10,assistance.context), "acknowledgements without actual motion grant no hold");
    assistance.motionObserved = true;
    check(assistance.canHold(10,assistance.context), "acknowledged progressing group can retain worker goal");
    auto changed = assistance.context; changed.site.x += 32;
    check(!assistance.canHold(10,changed), "changed construction site clears assistance");
    changed = assistance.context; ++changed.firstSeen;
    check(!assistance.canHold(10,changed), "reused builder ID clears assistance");
    changed = assistance.context; changed.waypoint.y += 32;
    check(!assistance.canHold(10,changed), "changed worker waypoint clears assistance");
    changed = assistance.context; ++changed.leaseIssued;
    check(!assistance.canHold(10,changed), "renewed construction lease clears old assistance");
    check(!assistance.canHold(23,assistance.context), "stale eligibility clears assistance");
    assistance.lastEligible = 59;
    check(!assistance.canHold(59,assistance.context), "no observed group progress for 49frames clears assistance");
    assistance.lastProgress = assistance.lastEligible = 297;
    check(assistance.canHold(297,assistance.context), "bounded assistance works before hard cap");
    assistance.lastProgress = assistance.lastEligible = 298;
    check(!assistance.canHold(298,assistance.context), "continuous helper motion cannot extend 288frame cap");
    assistance.closed = true;
    check(!assistance.canHold(11,assistance.context), "closed assistance cannot reopen indefinitely");
    std::cout << checked << " checked; " << failed << " failed\n";
    return failed ? 1 : 0;
}
