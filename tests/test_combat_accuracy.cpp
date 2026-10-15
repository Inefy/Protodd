#include "protodd/Combat.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Squads.hpp"
#include "protodd/UnitCatalog.hpp"
#include "protodd/UnitMemory.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <string_view>
#include <vector>

namespace {
using namespace protodd;
int failures{};
int checks{};
void check(const bool condition, const std::string_view message) {
    ++checks;
    if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot unit(const UnitId id, const UnitKind kind, const bool ours,
                  const Position position = {512, 512}) {
    UnitSnapshot result;
    result.id = id; result.kind = kind; result.ours = ours; result.position = position;
    result.visible = result.completed = result.powered = true;
    result.hitPoints = result.maxHitPoints = 1000;
    result.topSpeed = 4.0;
    return result;
}
double remainingValue(const UnitSnapshot& target, const int damage) {
    return unitStats(target.kind).combatValue *
        static_cast<double>(target.durability() - damage) /
        static_cast<double>(target.maxHitPoints + target.maxShields);
}

UnitSnapshot corridorActor(UnitId id,bool ours,Position p){UnitSnapshot u;u.id=id;u.kind=UnitKind::dragoon;u.ours=ours;u.position=p;u.visible=u.detected=u.completed=u.powered=true;u.hitPoints=u.maxHitPoints=100;u.shields=u.maxShields=80;u.topSpeed=4;u.weaponCooldown=20;u.groundWeapon={20,30,0,192,DamageType::explosive,false,true};u.airWeapon={20,30,0,192,DamageType::explosive,true,false};return u;}
bool corridorCrossesWeapon(Position from,Position to,const UnitSnapshot& weapon){const double dx=to.x-from.x,dy=to.y-from.y;const double length=dx*dx+dy*dy;const double t=length>0?std::clamp(((weapon.position.x-from.x)*dx+(weapon.position.y-from.y)*dy)/length,0.0,1.0):0;return std::hypot(from.x+t*dx-weapon.position.x,from.y+t*dy-weapon.position.y)<=weapon.groundWeapon.maxRange;}
void retreatCorridorSafety(){
 TacticalController controller;InfluenceMap influence;auto ours=corridorActor(1,true,{1024,720});auto foe=corridorActor(2,false,{1024,1024});const Position rally{1024,1280};const DefenseArea defense{rally,96,{-1,-1},{-1,-1}};
 CombatEstimate retreat;retreat.decision=FightDecision::retreat;retreat.ratio=.7;
 auto run=[&](const UnitSnapshot& unit,const std::span<const UnitSnapshot> enemies,const CombatEstimate& estimate,DefenseArea area=DefenseArea{}){return controller.control(std::array{unit},enemies,estimate,rally,rally,influence,unit.position,0,false,area);};
 auto near=run(ours,std::array{foe},retreat);
 check(near.size()==1 && near[0].source=="combat-retreat" && near[0].targetPosition.y<ours.position.y,"nearby live weapon forces an escape away from a forward rally");
 auto after=ours;if(near.size()==1)after.position=near[0].targetPosition;
 auto outside=run(after,std::array{foe},retreat);
 check(outside.size()==1 && !corridorCrossesWeapon(after.position,outside[0].targetPosition,foe),"crossing the nearby-threat buffer must not retreat through the same visible weapon");
 CombatEstimate kite;kite.decision=FightDecision::kite;kite.ratio=1.1;
 near=run(ours,std::array{foe},kite,defense);
 check(near.size()==1 && near[0].source=="combat-retreat" && near[0].targetPosition.y<ours.position.y,"healthy out-of-leash defender respects nearby visible fire");
 after=ours;if(near.size()==1)after.position=near[0].targetPosition;
 outside=run(after,std::array{foe},kite,defense);
 check(outside.size()==1 && !corridorCrossesWeapon(after.position,outside[0].targetPosition,foe),"crossing the nearby-threat buffer must not return through the same visible weapon");
 auto clear=run(after,{},kite,defense);
 check(clear.size()==1 && clear[0].source=="defense-return" && clear[0].targetPosition==rally,"removing the weapon permits an ordinary defense return");
 clear=run(after,{},retreat);
 check(clear.size()==1 && clear[0].targetPosition==rally,"clear retreat uses its strategic rally");
 auto incapable=foe;incapable.groundWeapon={};
 clear=run(after,std::array{incapable},kite,defense);
 check(clear.size()==1 && clear[0].source=="defense-return" && clear[0].targetPosition==rally,"air-only fire does not block a ground defender return");
 auto disabled=foe;disabled.disabled=true;
 clear=run(after,std::array{disabled},kite,defense);
 check(clear.size()==1 && clear[0].source=="defense-return" && clear[0].targetPosition==rally,"disabled weapons do not block a current defender return");
 for (const bool defending : {false,true}) {
  auto moving=ours;const auto estimate=defending?kite:retreat;bool safe=true;
  for(int i=0;i<48 && distance(moving.position,rally)>32;++i){
   auto orders=run(moving,std::array{foe},estimate,defending?defense:DefenseArea{});
   if(orders.size()!=1 || corridorCrossesWeapon(moving.position,orders[0].targetPosition,foe)){safe=false;break;}
   moving.position=orders[0].targetPosition;
  }
  check(safe,"bounded corridor steering avoids current visible weapon coverage throughout the detour");
  check(distance(moving.position,rally)<=32,"corridor steering reaches the strategic rally through a clear side");
 }
 auto body=ours;auto bodyFoe=foe;
 body.dimensionLeft=body.dimensionRight=body.dimensionUp=body.dimensionDown=16;
 bodyFoe.dimensionLeft=bodyFoe.dimensionRight=bodyFoe.dimensionUp=bodyFoe.dimensionDown=16;
 const Position bodyRally{1024,1408};bool bodySafe=true;
 for(int i=0;i<48 && distance(body.position,bodyRally)>32;++i){
  auto orders=controller.control(std::array{body},std::array{bodyFoe},retreat,bodyRally,bodyRally,influence,body.position);
  if(orders.size()!=1){bodySafe=false;break;}
  const auto destination=orders[0].targetPosition;const auto count=std::max(1,static_cast<int>(std::ceil(distance(body.position,destination)/4)));
  for(int sample=0;sample<=count;++sample){auto along=body;const auto fraction=static_cast<double>(sample)/count;along.position={static_cast<int>(std::lround(body.position.x+fraction*(destination.x-body.position.x))),static_cast<int>(std::lround(body.position.y+fraction*(destination.y-body.position.y)))};if(weaponDistance(along,bodyFoe)<=bodyFoe.groundWeapon.maxRange)bodySafe=false;}
  body.position=destination;
 }
 check(bodySafe,"body-aware detour keeps swept native weapon distance outside the visible firing range");
 check(distance(body.position,bodyRally)<=32,"body-aware corridor detour completes its strategic destination");
 std::vector<TacticalMovementEvidence> evidence;
 auto plain=controller.control(std::array{ours},std::array{foe},retreat,rally,rally,influence,ours.position,0,false,{},TacticalIntent::battle,{},nullptr,{},nullptr,true,{}, {},24);
 auto observed=controller.control(std::array{ours},std::array{foe},retreat,rally,rally,influence,ours.position,0,false,{},TacticalIntent::battle,{},nullptr,{},nullptr,true,{}, {},24,&evidence);
 check(plain.size()==observed.size() && plain.size()==1 && plain[0].type==observed[0].type && plain[0].targetPosition==observed[0].targetPosition && plain[0].source==observed[0].source,"passive movement evidence does not change current commands");
 check(evidence.size()==1 && evidence[0].actor==ours.id && evidence[0].origin==ours.position && evidence[0].destination==observed[0].targetPosition && evidence[0].steering && evidence[0].corridorThreat==foe.id,"movement evidence binds current origin, current weapon and proposed destination");
}

void defenderEscapePrecedence() {
    TacticalController controller;
    InfluenceMap influence;
    const DefenseArea defense{{1024,1280},96,{-1,-1},{-1,-1}};
    auto wounded=unit(1,UnitKind::zealot,true,{1024,1024});
    wounded.hitPoints=20;wounded.maxHitPoints=100;
    wounded.shields=0;wounded.maxShields=60;
    const std::array decisions{FightDecision::engage,FightDecision::retreat,FightDecision::engage};
    for (std::size_t i=0;i<decisions.size();++i) {
        CombatEstimate estimate;estimate.decision=decisions[i];estimate.ratio=i==1?0.5:2;
        wounded.position.y=1024-static_cast<int>(i)*48;
        const auto orders=controller.control(std::array{wounded},{},estimate,
            defense.center,{1024,768},influence,wounded.position,0,false,defense,
            TacticalIntent::battle,{},nullptr,{},nullptr,true,{}, {},24+24*i);
        check(orders.size()==1 && orders[0].source=="combat-retreat" &&
              orders[0].targetPosition.y<wounded.position.y,
              "critically wounded defenders keep escaping through engage/retreat/engage changes");
    }
    auto healthy=wounded;
    healthy.hitPoints=healthy.maxHitPoints;healthy.shields=healthy.maxShields;
    healthy.position={1024,1024};
    CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=2;
    auto orders=controller.control(std::array{healthy},{},engage,
        defense.center,{1024,768},influence,healthy.position,0,false,defense);
    check(orders.size()==1 && orders[0].source=="defense-return" &&
          orders[0].targetPosition==defense.center,
          "healthy out-of-leash defenders retain ordinary return authority");
    auto compromised=healthy;compromised.kind=UnitKind::darkTemplar;
    compromised.cloaked=true;compromised.recentlyDamaged=true;
    orders=controller.control(std::array{compromised},{},engage,
        defense.center,{1024,768},influence,compromised.position,0,false,defense);
    check(orders.size()==1 && orders[0].source=="combat-retreat" &&
          orders[0].targetPosition.y<compromised.position.y,
          "compromised cloak escapes are not redirected into a defense return");
    wounded.position={1024,1024};wounded.underStorm=true;
    orders=controller.control(std::array{wounded},{},engage,
        defense.center,{1024,768},influence,wounded.position,0,false,defense);
    check(orders.size()==1 && orders[0].source=="storm-escape" && orders[0].priority>=109,
          "critical-wound escape precedence preserves immediate storm evacuation");
}

void cadence() {
    check(engagementSimulationWithinBudget(0, 0) &&
          engagementSimulationWithinBudget(16, 12) &&
          engagementSimulationWithinBudget(24, 8),
          "bounded simulation preserves small and asymmetric engagements");
    check(!engagementSimulationWithinBudget(80, 116) &&
          !engagementSimulationWithinBudget(16, 16) &&
          !engagementSimulationWithinBudget(32, 1),
          "bounded simulation defers interactions beyond its callback work budget");
    auto attacker = unit(1, UnitKind::marine, true);
    auto target = unit(2, UnitKind::dragoon, false, {600, 512});
    attacker.groundWeapon = {5, 8, 0, 128, DamageType::normal, false, true};
    for (const auto cooldown : {7, 8, 15, 22, 30}) {
        for (const auto delay : {0, 5, 17}) {
            attacker.groundWeapon.cooldown = cooldown;
            attacker.weaponCooldown = delay;
            const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
            const auto hits = 1 + (336 - delay) / cooldown;
            check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, hits * 5)) < 1e-9,
                  "observed first-shot delay and weapon cadence retain individual frames");
        }
    }
    target.topSpeed = 0;
    target.position = {660, 512}; // 20 pixels outside range: first contact at frame five.
    attacker.groundWeapon.cooldown = 8;
    attacker.weaponCooldown = 2;
    auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, (1 + (336 - 5) / 8) * 5)) < 1e-9,
          "approach delays the first shot without accumulating free volleys");
    attacker.weaponCooldown = 17;
    result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, (1 + (336 - 17) / 8) * 5)) < 1e-9,
          "weapon reload still governs when contact occurs first");
    target.position = {4000, 512};
    result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(result.simulatedEnemyRemaining == unitStats(target.kind).combatValue,
          "contact beyond the simulation horizon cannot deal damage");
    check(result.simulatedFrames == 336 && !result.simulatedOutcomeReached,
          "unresolved engagements report the bounded horizon explicitly");

    auto first = unit(3, UnitKind::marine, true);
    first.hitPoints = first.maxHitPoints = 40;
    first.groundWeapon = {40, 15, 0, 128, DamageType::normal, false, true};
    auto second = first; second.id = 4; second.ours = false;
    result = CombatEvaluator{}.evaluate(std::array{first}, std::array{second}, 1, 0);
    check(result.simulatedFriendlyRemaining == 0 && result.simulatedEnemyRemaining == 0,
          "lethal volleys on the same frame remain simultaneous");
    second.weaponCooldown = 1;
    result = CombatEvaluator{}.evaluate(std::array{first}, std::array{second}, 1, 0);
    check(result.simulatedFriendlyRemaining > 0 && result.simulatedEnemyRemaining == 0,
          "a killed unit cannot return a volley on the following frame");
}
void boundedApproach() {
    auto runner = unit(1, UnitKind::marine, true, {272, 272});
    runner.groundWeapon = {5, 30, 0, 32, DamageType::normal, false, true};
    auto fleeing = unit(2, UnitKind::dragoon, false, {400, 272});
    fleeing.hitPoints = fleeing.maxHitPoints = 1000;
    fleeing.lastPosition = {380, 272};
    const auto sameSpeed = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{fleeing}, 1, 0);
    check(sameSpeed.simulatedEnemyRemaining == unitStats(fleeing.kind).combatValue,
          "a ranged target observed fleeing at equal speed is not pulled into range");
    runner.topSpeed = 8.0;
    const auto fasterChaser = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{fleeing}, 1, 0);
    check(fasterChaser.simulatedEnemyRemaining < unitStats(fleeing.kind).combatValue,
          "a faster pursuer can still close on a fleeing ranged target");

    constexpr int width = 24;
    constexpr int height = 16;
    constexpr int cell = 32;
    auto target = unit(10, UnitKind::dragoon, false, {400, 272});
    target.hitPoints = target.maxHitPoints = 5;
    target.groundWeapon = {};
    runner.topSpeed = 4.0;
    runner.hitPoints = runner.maxHitPoints = 1000;
    runner.groundWeapon = {5, 30, 0, 32, DamageType::normal, false, true};
    const auto openCells = std::vector<std::uint8_t>(width * height, 1);
    const NavigationGrid open{width, height, cell, openCells};
    const auto straight = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &open);

    auto detourCells = openCells;
    for (int y = 0; y < height; ++y)
        if (y != 1) detourCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid detour{width, height, cell, detourCells};
    const auto routed = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &detour);
    check(routed.simulatedOutcomeReached && routed.simulatedFrames > straight.simulatedFrames,
          "ground attackers reach a target through a bounded terrain route");

    auto blockedCells = openCells;
    for (int y = 0; y < height; ++y)
        blockedCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid blocked{width, height, cell, blockedCells};
    const auto unreachable = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{target}, 1, 0, true, &blocked);
    check(!unreachable.simulatedOutcomeReached &&
          unreachable.simulatedEnemyLoss == 0.0 &&
          (!unreachable.simulationRoutingDeferred || unreachable.confidence < 0.8),
          "a blocked or unproved ground approach grants no simulated damage or confident terrain outcome");
    auto disconnectedTarget = target;
    disconnectedTarget.position = {720, 272};
    const auto provenDisconnected = CombatEvaluator{}.evaluate(
        std::array{runner}, std::array{disconnectedTarget}, 1, 0, true, &blocked);
    check(!provenDisconnected.simulationRoutingDeferred &&
          !provenDisconnected.simulatedOutcomeReached &&
          provenDisconnected.simulatedEnemyRemaining == unitStats(target.kind).combatValue,
          "fully exhausted disconnected terrain retains a proven no-contact simulation");

    auto packed = std::array{runner, runner, runner};
    packed[0].id = 1;
    packed[1].id = 2;
    packed[2].id = 3;
    target.hitPoints = target.maxHitPoints = 15;
    const auto openChoke = CombatEvaluator{}.evaluate(
        packed, std::array{target}, 1, 0, true, &open);
    auto chokeCells = openCells;
    for (int y = 0; y < height; ++y)
        if (y != 8) chokeCells[static_cast<std::size_t>(y * width + 10)] = 0;
    const NavigationGrid choke{width, height, cell, chokeCells};
    const auto throughChoke = CombatEvaluator{}.evaluate(
        packed, std::array{target}, 1, 0, true, &choke);
    check(throughChoke.simulatedOutcomeReached &&
          throughChoke.simulatedFrames > openChoke.simulatedFrames,
          "a one-cell choke staggers ground attackers instead of granting immediate group DPS");
    std::cout << "bounded approach frames: straight=" << straight.simulatedFrames
              << " detour=" << routed.simulatedFrames
              << " open-choke=" << openChoke.simulatedFrames
              << " one-cell-choke=" << throughChoke.simulatedFrames << '\n';

    auto ranged = runner;
    ranged.position = {272, 272};
    ranged.groundWeapon.maxRange = 160;
    target.hitPoints = target.maxHitPoints = 5;
    const auto beforeRanged = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto rangedAcrossWall = CombatEvaluator{}.evaluate(
        std::array{ranged}, std::array{target}, 1, 0, true, &blocked);
    check(rangedAcrossWall.simulatedOutcomeReached && rangedAcrossWall.simulatedFrames == 0 &&
          !rangedAcrossWall.simulationRoutingDeferred &&
          NavigationGrid::diagnosticsForCurrentThread().searches == beforeRanged,
          "an observed legal in-range ranged shot crosses impassable ground without routing");

    NavigationGrid largeOpen(512, 448, 8, std::vector<std::uint8_t>(512 * 448, 1));
    std::vector<UnitSnapshot> manyAttackers(12, runner);
    std::vector<UnitSnapshot> manyDefenders(12, target);
    for (int index = 0; index < 12; ++index) {
        manyAttackers[index].id = 100 + index;
        manyAttackers[index].position = {272, 272 + index * 8};
        manyDefenders[index].id = 200 + index;
        manyDefenders[index].position = {1008, 272 + index * 8};
        manyDefenders[index].hitPoints = manyDefenders[index].maxHitPoints = 1000;
    }
    const auto boundedGeometry = CombatEvaluator{}.evaluate(
        manyAttackers, manyDefenders, 1, 0, true, &largeOpen);
    const auto staticGeometry = CombatEvaluator{}.evaluate(
        manyAttackers, manyDefenders, 1, 0, false, &largeOpen);
    check(boundedGeometry.simulationRoutingDeferred &&
          boundedGeometry.simulatedFrames == 0 && !boundedGeometry.simulatedOutcomeReached &&
          boundedGeometry.simulatedFriendlyDeaths == 0 && boundedGeometry.simulatedEnemyDeaths == 0 &&
          boundedGeometry.ratio == staticGeometry.ratio &&
          boundedGeometry.confidence == staticGeometry.confidence &&
          boundedGeometry.decision == staticGeometry.decision,
          "exhausted aggregate clearance work discards partial damage and uses the low-confidence static fallback");

    std::vector<std::uint8_t> largeBlockedCells(512 * 448, 1);
    for (int y = 0; y < 448; ++y) largeBlockedCells[y * 512 + 256] = 0;
    NavigationGrid largeBlocked(512, 448, 8, std::move(largeBlockedCells));
    manyAttackers.front().position = {1008, 272};
    manyDefenders.front().position = {3008, 272};
    manyDefenders.front().groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    const auto beforeBounded = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto incompleteRoute = CombatEvaluator{}.evaluate(
        std::span{manyAttackers}.first(1), std::span{manyDefenders}.first(1),
        1, 0, true, &largeBlocked);
    check(incompleteRoute.simulationRoutingDeferred && incompleteRoute.simulatedFrames == 0 &&
          incompleteRoute.confidence < 0.8 &&
          NavigationGrid::diagnosticsForCurrentThread().searches - beforeBounded <=
              maximumEngagementRouteSearches,
          "an incomplete bounded terrain search reduces authority instead of proving the opponent harmless");
}
void combatStatusAdmissibility() {
    CombatEvaluator evaluator;
    auto dragoon = unit(1, UnitKind::dragoon, true);
    dragoon.groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    auto darkTemplar = unit(2, UnitKind::darkTemplar, false, {560, 512});
    darkTemplar.cloaked = true;
    darkTemplar.detected = false;
    darkTemplar.groundWeapon = {40, 30, 0, 128, DamageType::normal, false, true};
    const auto hiddenStatic = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0, false);
    const auto hiddenFight = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0);
    check(hiddenStatic.friendlyPower == 0.0,
          "static power does not credit fire against an undetected cloaked target");
    check(hiddenFight.simulatedEnemyRemaining == unitStats(darkTemplar.kind).combatValue,
          "simulation leaves an undetected cloaked enemy untargeted after detector loss");
    darkTemplar.detected = true;
    const auto detectedFight = evaluator.evaluate(
        std::array{dragoon}, std::array{darkTemplar}, 1, 0);
    check(detectedFight.friendlyPower > 0.0 &&
          detectedFight.simulatedEnemyRemaining < unitStats(darkTemplar.kind).combatValue,
          "restored detection returns the cloaked target to legal fire and damage");

    auto ordinary = darkTemplar;
    ordinary.kind = UnitKind::dragoon;
    ordinary.cloaked = false;
    const auto visibleOrdinary = evaluator.evaluate(
        std::array{dragoon}, std::array{ordinary}, 1, 0);
    const auto fogged = reconcileEnemyMemory(ordinary, std::nullopt, 24, false, false).snapshot;
    const auto rememberedFight = evaluator.evaluate(
        std::array{dragoon}, std::array{fogged}, 1, 0);
    const auto rememberedStatic = evaluator.evaluate(
        std::array{dragoon}, std::array{fogged}, 1, 0, false);
    check(!fogged.visible && !fogged.detected && !fogged.requiresDetection() &&
          rememberedStatic.friendlyPower > 0.0 && rememberedStatic.enemyPower > 0.0 &&
          rememberedFight.friendlyPower == visibleOrdinary.friendlyPower &&
          rememberedFight.enemyPower == visibleOrdinary.enemyPower &&
          rememberedFight.simulatedEnemyRemaining == visibleOrdinary.simulatedEnemyRemaining,
          "ordinary fog memory preserves prospective combat on both sides instead of requiring detection");
    const auto hiddenCommands = TacticalController{}.control(
        std::array{dragoon}, std::array{fogged}, rememberedFight,
        {900, 512}, {400, 512}, InfluenceMap{});
    check(std::ranges::none_of(hiddenCommands, [&fogged](const Command& command) {
              return command.type == CommandType::attackUnit && command.targetUnit == fogged.id;
          }), "prospective fog combat never authorizes a hidden attack target");
    for (const auto kind : {UnitKind::darkTemplar, UnitKind::lurker, UnitKind::spiderMine}) {
        auto covert = ordinary;
        covert.kind = kind;
        covert.detected = false;
        covert.groundWeapon = {}; // Isolate our fire from a mine's own suicide impact.
        const auto memory = reconcileEnemyMemory(covert, std::nullopt, 24, false, false).snapshot;
        const auto estimate = evaluator.evaluate(std::array{dragoon}, std::array{memory}, 1, 0);
        check(memory.requiresDetection() && estimate.friendlyPower == 0.0 &&
              estimate.simulatedEnemyRemaining == unitStats(kind).combatValue,
              "remembered inherently covert targets still block predicted fire without detection");
    }

    auto marine = unit(10, UnitKind::marine, true);
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto opponent = marine;
    opponent.id = 11;
    opponent.ours = false;
    const auto checkDisabledEnemy = [&](const UnitKind kind,
                                        const std::string_view effect) {
        auto disabled = opponent;
        disabled.kind = kind;
        disabled.disabled = true; // The BWAPI adapter normalizes all three effects here.
        const auto estimate = evaluator.evaluate(std::array{marine}, std::array{disabled}, 1, 0);
        check(estimate.enemyPower == 0.0 && estimate.simulatedFriendlyRemaining > 0.0,
              effect);
    };
    checkDisabledEnemy(UnitKind::marine,
        "a stasised opponent adds no defensive damage");
    checkDisabledEnemy(UnitKind::siegeTank,
        "a locked-down mechanical opponent adds no defensive damage");
    checkDisabledEnemy(UnitKind::dragoon,
        "a maelstrommed opponent adds no defensive damage");
    auto disabledMarine = marine;
    disabledMarine.disabled = true;
    const auto disabledFriendly = evaluator.evaluate(
        std::array{disabledMarine}, std::array{opponent}, 1, 0);
    check(disabledFriendly.friendlyPower == 0.0 &&
          disabledFriendly.simulatedEnemyRemaining == unitStats(opponent.kind).combatValue,
          "a disabled friendly unit contributes no fictitious offensive coverage");

    auto cannon = unit(20, UnitKind::photonCannon, true);
    cannon.groundWeapon = {20, 30, 0, 128, DamageType::normal, false, true};
    auto target = unit(21, UnitKind::marine, false, {560, 512});
    target.hitPoints = target.maxHitPoints = 5;
    target.groundWeapon = {};
    const auto powered = evaluator.evaluate(std::array{cannon}, std::array{target}, 1, 0);
    check(powered.friendlyPower > 0.0 && powered.simulatedEnemyRemaining == 0.0,
          "a powered Cannon contributes defensive coverage and fires in the simulation");
    cannon.powered = false;
    const auto unpowered = evaluator.evaluate(std::array{cannon}, std::array{target}, 1, 0);
    check(unpowered.friendlyPower == 0.0 && unpowered.simulatedEnemyRemaining ==
              unitStats(target.kind).combatValue,
          "power loss removes Cannon threat from both static and simulated combat");
}
void splashAndSpellCalibration() {
    auto reaver = unit(1, UnitKind::reaver, true, {512, 512});
    reaver.ammo = 1;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    reaver.groundWeapon.splashInner = 12;
    reaver.groundWeapon.splashMiddle = 28;
    reaver.groundWeapon.splashOuter = 48;
    std::vector<UnitSnapshot> packed;
    for (int index = 0; index < 3; ++index) {
        auto target = unit(10 + index, UnitKind::zergling, false,
                           {620 + index * 16, 512});
        target.hitPoints = target.maxHitPoints = 35;
        target.topSpeed = 0.0;
        target.groundWeapon = {};
        packed.push_back(target);
    }
    auto spread = packed;
    spread[1].position.x += 160;
    spread[2].position.x += 320;
    const auto clusteredImpact = CombatEvaluator{}.evaluate(
        std::array{reaver}, packed, 1, 0);
    const auto spreadImpact = CombatEvaluator{}.evaluate(
        std::array{reaver}, spread, 1, 0);
    check(clusteredImpact.simulatedEnemyLoss > spreadImpact.simulatedEnemyLoss,
          "engine-calibrated splash bands improve damage only when enemies are clustered");

    auto tank = unit(20, UnitKind::siegeTank, true, {512, 512});
    tank.groundWeapon = {40, 30, 0, 256, DamageType::normal, false, true};
    tank.groundWeapon.splashInner = 10;
    tank.groundWeapon.splashMiddle = 25;
    tank.groundWeapon.splashOuter = 40;
    tank.groundWeapon.splashFriendlyFire = true;
    auto stationaryAlly = unit(21, UnitKind::photonCannon, true, {620, 512});
    stationaryAlly.topSpeed = 0.0;
    stationaryAlly.groundWeapon = {};
    auto enemy = unit(22, UnitKind::marine, false, {600, 512});
    enemy.groundWeapon = {};
    const auto radial = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    tank.groundWeapon.splashFriendlyFire = false;
    const auto enemyOnly = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    check(radial.simulatedFriendlyLoss > 0.0 && enemyOnly.simulatedFriendlyLoss == 0.0,
          "Radial_Splash accounts for friendly exposure while Enemy_Splash does not");
    stationaryAlly.position.x = 700;
    tank.groundWeapon.splashFriendlyFire = true;
    const auto separatedAlly = CombatEvaluator{}.evaluate(
        std::array{tank, stationaryAlly}, std::array{enemy}, 1, 0);
    check(separatedAlly.simulatedFriendlyLoss == 0.0,
          "spread allies outside the engine outer radius avoid splash damage");

    auto templar = unit(30, UnitKind::highTemplar, true);
    templar.role = UnitRole::spellcaster;
    templar.energy = 200;
    auto marines = std::array{unit(31, UnitKind::marine, false),
                              unit(32, UnitKind::marine, false)};
    for (auto& marine : marines) {
        marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    }
    const auto theoreticalStorm = CombatEvaluator{}.evaluate(
        std::array{templar}, marines, 1.2, 0, false);
    check(theoreticalStorm.friendlyPower == 0.0 &&
          theoreticalStorm.decision == FightDecision::retreat,
          "unmodeled spell value alone cannot turn a losing force into an engagement");
}
void outcomeMetrics() {
    auto attacker = unit(1, UnitKind::marine, true);
    attacker.hitPoints = attacker.maxHitPoints = 40;
    attacker.weaponCooldown = 2;
    attacker.groundWeapon = {10, 5, 0, 128, DamageType::normal, false, true};
    auto target = unit(2, UnitKind::dragoon, false);
    target.hitPoints = target.maxHitPoints = 30;
    target.groundWeapon = {};

    const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
    check(result.simulatedFrames == 12 && result.simulatedOutcomeReached,
          "prediction records the frame when the final target is killed");
    check(result.simulatedEnemyDeaths == 1 && result.simulatedFriendlyDeaths == 0,
          "prediction separates unit casualties from health loss");
    check(std::abs(result.simulatedEnemyLoss - unitStats(target.kind).combatValue) < 1e-9 &&
          result.simulatedFriendlyLoss == 0,
          "prediction reports casualty value and zero attacker losses");

    const auto staticOnly = CombatEvaluator{}.evaluate(
        std::array{attacker}, std::array{target}, 1, 0, false);
    check(staticOnly.simulatedFrames == 0 && !staticOnly.simulatedOutcomeReached &&
          staticOnly.simulatedEnemyDeaths == 0 && staticOnly.simulatedEnemyLoss == 0,
          "static-only estimates do not fabricate simulation outcomes");
}
void truncationCoverage() {
    auto fighters = std::vector<UnitSnapshot>{};
    fighters.reserve(97);
    for (int index = 0; index < 96; ++index) {
        auto dragoon = unit(index + 1, UnitKind::dragoon, true,
                            {400 + (index % 12) * 20, 400 + (index / 12) * 20});
        dragoon.role = UnitRole::groundArmy;
        dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        // This formation has no represented anti-air despite the Dragoon label.
        dragoon.airWeapon = {};
        fighters.push_back(dragoon);
    }
    auto scout = unit(200, UnitKind::scout, false, {640, 512});
    scout.flying = true;
    scout.groundWeapon = {};
    scout.airWeapon = {8, 30, 0, 192, DamageType::normal, true, false};
    const auto uncapped = CombatEvaluator{}.evaluate(fighters, std::array{scout}, 1, 0);

    auto corsair = unit(97, UnitKind::corsair, true, {400, 400});
    corsair.role = UnitRole::airArmy;
    corsair.groundWeapon = {};
    corsair.airWeapon = {5, 10, 0, 192, DamageType::normal, true, false};
    fighters.push_back(corsair);
    const auto capped = CombatEvaluator{}.evaluate(fighters, std::array{scout}, 1, 0);
    const auto airArmyRole = std::uint32_t{1} << static_cast<std::uint32_t>(UnitRole::airArmy);
    check(capped.omittedFriendly.unitCount == 1 &&
          capped.omittedFriendly.combatValue > 0.0,
          "the 97th eligible fighter is explicitly counted with omitted combat value");
    check((capped.omittedFriendly.roleMask & airArmyRole) != 0U &&
          capped.omittedFriendly.airWeaponValue > 0.0,
          "truncation reports the omitted role and anti-air weapon domain");
    check(capped.confidence < uncapped.confidence * 0.6,
          "an omitted sole anti-air counter sharply lowers decision confidence");

    auto spellForce = std::vector<UnitSnapshot>(fighters.begin(), fighters.begin() + 96);
    auto groundEnemy = unit(201, UnitKind::marine, false, {640, 512});
    groundEnemy.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    const auto noOmittedSupport = CombatEvaluator{}.evaluate(
        spellForce, std::array{groundEnemy}, 1, 0);
    auto templar = unit(97, UnitKind::highTemplar, true, {400, 400});
    templar.role = UnitRole::spellcaster;
    templar.energy = 150;
    templar.groundWeapon = {};
    templar.airWeapon = {};
    spellForce.push_back(templar);
    const auto omittedSupport = CombatEvaluator{}.evaluate(
        spellForce, std::array{groundEnemy}, 1, 0);
    const auto spellcasterRole = std::uint32_t{1} <<
                                 static_cast<std::uint32_t>(UnitRole::spellcaster);
    check((omittedSupport.omittedFriendly.roleMask & spellcasterRole) != 0U &&
          omittedSupport.confidence < noOmittedSupport.confidence,
          "an omitted energized spellcaster support role further reduces authority");
}
void confidencePolicy() {
    auto marine = unit(1, UnitKind::marine, true);
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto equalMarine = marine;
    equalMarine.id = 2;
    equalMarine.ours = false;
    equalMarine.position = {600, 512};
    const auto staged = CombatEvaluator{}.evaluate(
        std::array{marine}, std::array{equalMarine}, 0.8, 0.6, false);
    check(staged.confidence < 0.8 && staged.ratio >= 0.8 &&
          staged.decision == FightDecision::kite && staged.confidenceStaged,
          "a low-confidence near-equal static estimate stages as kite for reassessment");

    std::array<UnitSnapshot, 4> advantaged{};
    for (int index = 0; index < static_cast<int>(advantaged.size()); ++index) {
        auto zealot = unit(10 + index, UnitKind::zealot, true,
                           {512 + index * 16, 512});
        zealot.groundWeapon = {8, 22, 0, 15, DamageType::normal, false, true};
        advantaged[static_cast<std::size_t>(index)] = zealot;
    }
    const auto decisive = CombatEvaluator{}.evaluate(
        advantaged, std::array{equalMarine}, 1.0, 0.9, false);
    check(decisive.confidence < 0.8 && decisive.ratio >= 1.25 &&
          decisive.decision == FightDecision::engage && !decisive.confidenceStaged,
          "decisive legal weapon advantage can still commit under low confidence");

    auto emptyReavers = std::vector<UnitSnapshot>{};
    for (int index = 0; index < 34; ++index) {
        auto reaver = unit(100 + index, UnitKind::reaver, true,
                           {512 + index * 8, 512});
        reaver.hitPoints = 1;
        reaver.ammo = 0;
        reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
        emptyReavers.push_back(reaver);
    }
    auto lethalContact = unit(300, UnitKind::zergling, false, {560, 512});
    lethalContact.groundWeapon = {200, 13, 0, 128, DamageType::normal, false, true};
    const auto uncertainLoss = CombatEvaluator{}.evaluate(
        emptyReavers, std::array{lethalContact}, 1.0, 0.5, true);
    check(uncertainLoss.confidence < 0.8 && uncertainLoss.ratio < 0.72 &&
          uncertainLoss.decision == FightDecision::kite &&
          uncertainLoss.confidenceStaged,
          "a fragile fight estimate probes instead of hard-retreating against decisive static power");

    const auto noDecisiveEvidence = CombatEvaluator{}.evaluate(
        std::array{marine}, std::array{equalMarine}, 1.2, 0.6, false);
    check(noDecisiveEvidence.decision == FightDecision::retreat &&
          !noDecisiveEvidence.confidenceStaged,
          "low confidence alone does not invent a probe without decisive legal power");
}
void consumableAttackers() {
    for (const auto kind : {UnitKind::scourge, UnitKind::spiderMine, UnitKind::infestedTerran}) {
        auto attacker = unit(1, kind, true);
        attacker.flying = kind == UnitKind::scourge;
        attacker.groundWeapon = {110, 1, 0, 32, DamageType::normal, false, true};
        attacker.airWeapon = {110, 1, 0, 32, DamageType::normal, true, false};
        auto target = unit(2, UnitKind::dragoon, false);
        target.flying = attacker.flying;
        const auto result = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
        check(result.simulatedFriendlyRemaining == 0, "suicide attack consumes its attacker");
        check(std::abs(result.simulatedEnemyRemaining - remainingValue(target, 110)) < 1e-9,
              "suicide attacker contributes exactly one impact");
        const auto fast = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0, false);
        check(fast.friendlyPower < unitStats(kind).combatValue * 2,
              "static estimate does not interpret suicide cooldown as sustained damage");
        target.invincible = true;
        const auto noShot = CombatEvaluator{}.evaluate(std::array{attacker}, std::array{target}, 1, 0);
        check(noShot.simulatedFriendlyRemaining == unitStats(kind).combatValue,
              "suicide unit survives when no legal impact occurs");
    }
}
void workerDefense() {
    auto raider = unit(1, UnitKind::zealot, true);
    auto worker = unit(2, UnitKind::scv, false);
    worker.groundWeapon = {5, 15, 0, 32, DamageType::normal, false, true};
    const auto result = CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0);
    check(result.enemyPower > 0, "local worker weapons contribute defensive power");
    check(result.simulatedFriendlyRemaining < unitStats(raider.kind).combatValue,
          "a worker surround is not simulated as a harmless mineral line");
    raider.flying = true;
    check(CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0).enemyPower == 0,
          "workers do not create an anti-air threat");
    raider.flying = false;
    worker.loaded = true;
    check(CombatEvaluator{}.evaluate(std::array{raider}, std::array{worker}, 1, 0).enemyPower == 0,
          "loaded workers cannot contribute defense");
    worker.loaded = false;
    GameState state;
    state.frame = 5000;
    state.self.units = {raider};
    state.enemy.units = {worker};
    StrategicPlan plan; plan.attackTarget = {900, 512};
    const auto defenders = [&]() {
        // The live adapter supplies an army-only enemy span separately from
        // the complete legal observations, including workers, in GameState.
        const auto squads = SquadPlanner{}.form(state, state.self.units, {}, plan, {100, 512});
        return squads.empty() ? std::vector<UnitSnapshot>{} : squads.front().enemies;
    };
    check(defenders().size() == 1, "live squad formation includes an immediate worker surround");
    state.enemy.units[0].position.x += 140;
    check(defenders().empty(), "distant mining workers do not inflate the local enemy army");
    state.enemy.units[0].orderTargetId = raider.id;
    check(defenders().size() == 1, "visible workers approaching a squad member to attack count as defenders");
    state.enemy.units[0].visible = false;
    check(defenders().empty(), "fogged workers cannot create a speculative surround");
    state.enemy.units[0] = worker;
    state.self.units[0].flying = true;
    check(defenders().empty(), "worker defense cannot pin an air squad");
}
void unavailableThreats() {
    auto goon = unit(1, UnitKind::dragoon, true);
    goon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    goon.weaponCooldown = 20;
    auto neighbor = goon; neighbor.id = 2; neighbor.position.y += 32;
    auto target = unit(10, UnitKind::probe, false, {680, 512});
    auto threat = unit(11, UnitKind::reaver, false, {650, 512});
    threat.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    threat.ammo = 5;
    CombatEstimate engage; engage.decision = FightDecision::engage; engage.ratio = 2;
    const auto commands = [&](const UnitSnapshot& candidate) {
        return TacticalController{}.control(std::array{goon, neighbor}, std::array{target, candidate},
            engage, {800, 512}, {200, 512}, InfluenceMap{}, goon.position, 3);
    };
    const auto hasSpacing = [](const std::vector<Command>& orders) {
        return std::ranges::any_of(orders, [](const Command& c) { return c.source == "splash-spacing"; });
    };
    check(hasSpacing(commands(threat)), "armed Reaver still triggers splash spacing");
    for (int variant = 0; variant < 4; ++variant) {
        auto unavailable = threat;
        if (variant == 0) unavailable.loaded = true;
        if (variant == 1) unavailable.hallucination = true;
        if (variant == 2) unavailable.ammo = 0;
        if (variant == 3) unavailable.invincible = true;
        check(!hasSpacing(commands(unavailable)), "unavailable splash cannot scatter the frontline");
    }
    engage.decision = FightDecision::kite;
    threat.kind = UnitKind::marine;
    threat.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    const auto hasKite = [](const std::vector<Command>& orders) {
        return std::ranges::any_of(orders, [](const Command& c) { return c.source == "combat-kite"; });
    };
    check(hasKite(commands(threat)), "real pursuer still triggers reload kiting");
    threat.loaded = true;
    check(!hasKite(commands(threat)), "loaded passenger cannot cause reload kiting");
    threat.loaded = false; threat.hallucination = true;
    check(!hasKite(commands(threat)), "hallucinated pursuer cannot cause reload kiting");
    threat.hallucination = false; threat.kind = UnitKind::photonCannon;
    threat.powered = false;
    check(!hasKite(commands(threat)), "unpowered Cannon cannot cause reload kiting");
}
void terrainSpacing() {
    auto goon = unit(1, UnitKind::dragoon, true, {496, 496});
    goon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    goon.weaponCooldown = 20;
    auto neighbor = goon; neighbor.id = 2; neighbor.position.y += 32;
    auto reaver = unit(10, UnitKind::reaver, false, {680, 496});
    reaver.ammo = 5;
    reaver.groundWeapon = {100, 60, 0, 256, DamageType::normal, false, true};
    std::vector<std::uint8_t> cells(32 * 32, 0);
    cells[15 * 32 + 15] = 1; // Only the observed origin is traversable.
    NavigationGrid nav{32, 32, 32, cells};
    CombatEstimate engage; engage.decision = FightDecision::engage;
    const auto orders = TacticalController{}.control(std::array{goon, neighbor}, std::array{reaver},
        engage, {800, 496}, {200, 496}, InfluenceMap{}, goon.position, 3, false, {},
        TacticalIntent::battle, {}, &nav);
    check(std::ranges::none_of(orders, [&nav, &goon](const Command& command) {
        return command.actor == goon.id && command.source == "splash-spacing" &&
            !nav.lineWalkable(goon.position, command.targetPosition);
    }), "splash spacing cannot choose a blocked terrain segment");

    // The far end of this wall has an opening. A global hasPath check accepts
    // the old preferred destination, but a short reload move cannot cross it.
    cells.assign(32 * 32, 1);
    for (int x = 0; x < 31; ++x) cells[14 * 32 + static_cast<std::size_t>(x)] = 0;
    nav = NavigationGrid{32, 32, 32, cells};
    check(nav.findPath(goon.position, {496, 432}).reached() &&
          !nav.lineWalkable(goon.position, {496, 432}), "cliff fixture has a distant legal detour");
    const auto rerouted = TacticalController{}.control(std::array{goon, neighbor}, std::array{reaver},
        engage, {800, 496}, {200, 496}, InfluenceMap{}, goon.position, 3, false, {},
        TacticalIntent::battle, {}, &nav);
    check(std::ranges::any_of(rerouted, [&nav, &goon](const Command& command) {
        return command.actor == goon.id && command.source == "splash-spacing" &&
            nav.lineWalkable(goon.position, command.targetPosition);
    }), "splash spacing chooses a reachable alternative beside the cliff");
}
void deterministicScheduling() {
    auto a = unit(1, UnitKind::dragoon, true);
    a.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto b = a; b.id = 2; b.weaponCooldown = 5;
    auto x = unit(10, UnitKind::marine, false, {600, 512});
    x.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    auto y = x; y.id = 11; y.weaponCooldown = 7;
    CombatEvaluator evaluator;
    const auto original = evaluator.evaluate(std::array{a, b}, std::array{x, y}, 1, 0);
    const auto reordered = evaluator.evaluate(std::array{b, a}, std::array{y, x}, 1, 0);
    const auto mixedFriendly = evaluator.evaluate(std::array{b, a}, std::array{x, y}, 1, 0);
    const auto mixedEnemy = evaluator.evaluate(std::array{a, b}, std::array{y, x}, 1, 0);
    const auto samePrediction = [](const CombatEstimate& left, const CombatEstimate& right) {
        const auto sameOmissions = [](const SimulationOmissions& a, const SimulationOmissions& b) {
            return a.combatValue == b.combatValue && a.unitCount == b.unitCount &&
                a.roleMask == b.roleMask && a.groundWeaponValue == b.groundWeaponValue &&
                a.airWeaponValue == b.airWeaponValue;
        };
        return left.friendlyPower == right.friendlyPower && left.enemyPower == right.enemyPower &&
            left.ratio == right.ratio && left.confidence == right.confidence &&
            left.simulatedEnemyRemaining == right.simulatedEnemyRemaining &&
            left.simulatedFriendlyRemaining == right.simulatedFriendlyRemaining &&
            left.simulatedEnemyLoss == right.simulatedEnemyLoss &&
            left.simulatedFriendlyLoss == right.simulatedFriendlyLoss &&
            left.simulatedFrames == right.simulatedFrames &&
            left.simulatedFriendlyDeaths == right.simulatedFriendlyDeaths &&
            left.simulatedEnemyDeaths == right.simulatedEnemyDeaths &&
            left.simulatedOutcomeReached == right.simulatedOutcomeReached &&
            left.confidenceStaged == right.confidenceStaged &&
            sameOmissions(left.omittedFriendly, right.omittedFriendly) &&
            sameOmissions(left.omittedEnemy, right.omittedEnemy) &&
            left.decision == right.decision && left.advanceBlocked == right.advanceBlocked &&
            left.holdScreen == right.holdScreen;
    };
    check(samePrediction(original, reordered) && samePrediction(original, mixedFriendly) &&
          samePrediction(original, mixedEnemy),
          "all friendly/enemy input permutations preserve every public engine-case prediction");
    a.groundWeapon = {}; x.groundWeapon = {};
    const auto passive = evaluator.evaluate(std::array{a}, std::array{x}, 1, 0);
    check(passive.simulatedFriendlyRemaining == unitStats(a.kind).combatValue &&
          passive.simulatedEnemyRemaining == unitStats(x.kind).combatValue,
          "an encounter with no eligible weapon events preserves both armies");

    auto tieAttacker = unit(20, UnitKind::dragoon, true);
    tieAttacker.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto lowerId = unit(31, UnitKind::marine, false, {600, 512});
    auto higherId = lowerId;
    higherId.id = 32;
    const auto lowerFirstOrder = std::array{lowerId, higherId};
    const auto higherFirstOrder = std::array{higherId, lowerId};
    const auto lowerFirst = evaluator.selectTarget(tieAttacker, lowerFirstOrder);
    const auto higherFirst = evaluator.selectTarget(tieAttacker, higherFirstOrder);
    check(lowerFirst != nullptr && higherFirst != nullptr &&
          lowerFirst->id == 31 && higherFirst->id == lowerFirst->id,
          "equal-score target comparator resolves to the same lower unit ID in either order");
}

void dragoonFiringCycle() {
    auto dragoon = unit(1, UnitKind::dragoon, true, {500, 500});
    dragoon.hitPoints = dragoon.maxHitPoints = 100;
    dragoon.maxShields = dragoon.shields = 80;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    auto marine = unit(2, UnitKind::marine, false, {850, 500});
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    CombatEstimate kite;
    kite.decision = FightDecision::kite;
    CombatEstimate retreat;
    retreat.decision = FightDecision::retreat;
    TacticalController controller;
    InfluenceMap influence;

    for (const auto latency : {0, 3, 6, 12, 24}) {
        auto orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().source == "focus-fire",
              "a Dragoon starts an attack approach to a moving target outside firing range");

        marine.position = {650 + latency, 512};
        dragoon.weaponCooldown = 0;
        dragoon.attackWindup = true;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.empty(), "windup is allowed to finish before kiting or retargeting");
        dragoon.attackWindup = false;
        dragoon.attackFrame = true;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.empty(), "the projectile release attack frame cannot be interrupted by movement");

        dragoon.attackFrame = false;
        dragoon.weaponCooldown = latency + 3;
        marine.position.x += 8;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::move &&
              orders.front().source == "combat-kite",
              "post-release movement uses reload time against a moving Marine target");

        dragoon.weaponCooldown = latency + 2;
        marine.position.x += 8;
        orders = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().targetUnit == marine.id && orders.front().source == "focus-fire",
              "the Dragoon resumes its volley at the latency-aware ready boundary");

        dragoon.weaponCooldown = 0;
        orders = controller.control(std::array{dragoon}, std::array{marine}, retreat,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
              orders.front().source == "retreat-volley",
              "retreat preserves an immediately available Dragoon shot at every latency");
        dragoon.weaponCooldown = latency + 3;
        orders = controller.control(std::array{dragoon}, std::array{marine}, retreat,
            marine.position, {400, 500}, influence, {}, latency);
        check(orders.size() == 1 && orders.front().type == CommandType::move &&
              orders.front().source == "combat-retreat",
              "retreat movement resumes between Dragoon volleys");
    }

    // Model a path-stuck Dragoon whose Attack_Unit order never completes.
    // The bridge reports that order inactive, while CommandBus bounds retry
    // cadence so the actor can recover without resending every frame.
    marine.position = {650, 500};
    dragoon.weaponCooldown = 0;
    CommandBus bus;
    std::vector<Frame> attackFrames;
    for (Frame frame = 0; frame <= 60; ++frame) {
        marine.position.x = 650 + static_cast<int>(frame % 3) * 2;
        const auto proposed = controller.control(std::array{dragoon}, std::array{marine}, kite,
            marine.position, {400, 500}, influence, {}, 6);
        bus.beginFrame(frame, 6);
        for (const auto& command : proposed) bus.submit(command);
        const auto issued = bus.finalize();
        for (const auto& command : issued) {
            if (command.type == CommandType::attackUnit) attackFrames.push_back(frame);
            bus.markIssued(command);
        }
    }
    check(attackFrames == std::vector<Frame>{0, 19, 38, 57},
          "a stuck Dragoon retries after bounded suppression instead of flooding attack orders");
}





void rangedSupportProofControls() {
    NavigationGrid grid(64,64,16,std::vector<std::uint8_t>(64*64,1));
    auto ally=unit(100,UnitKind::zealot,true,{600,480});
    ally.dimensionLeft=ally.dimensionRight=11;ally.dimensionUp=5;ally.dimensionDown=13;
    ally.groundWeapon={16,22,0,15,DamageType::normal,false,true};
    auto actor=unit(200,UnitKind::dragoon,true,{400,480});
    actor.dimensionLeft=actor.dimensionUp=15;actor.dimensionRight=actor.dimensionDown=16;
    actor.groundWeapon={20,30,0,192,DamageType::explosive,false,true};
    auto target=unit(50,UnitKind::hydralisk,false,{800,480});
    target.dimensionLeft=target.dimensionRight=target.dimensionUp=target.dimensionDown=16;
    CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=100;
    InfluenceMap influence;TacticalController controller;
    const auto run=[&](const NavigationGrid& nav,Frame frame){
        return controller.control(std::array{actor},std::array{target},engage,target.position,
            {200,480},influence,actor.position,0,false,{},TacticalIntent::battle,
            std::array{ally},&nav,{},nullptr,true,{},target.position,frame);
    };
    const auto before=NavigationGrid::diagnosticsForCurrentThread();
    const auto direct=run(grid,100);
    const auto after=NavigationGrid::diagnosticsForCurrentThread();
    check(direct.size()==1 && direct[0].source=="melee-support-hold" &&
              after.searches==before.searches,
          "a body-clear supporting melee route uses a direct proof without A-star");
    grid.updateDynamicObstacle(1,{696,0,712,1023});
    const auto blocked=run(grid,112);
    check(blocked.size()==1 && blocked[0].source=="focus-fire",
          "a former support proof cannot survive a new blocking obstacle next pass");
    grid.removeDynamicObstacle(1);
    const auto reopened=run(grid,124);
    check(reopened.size()==1 && reopened[0].source=="melee-support-hold",
          "an unproved support route is rechecked when geometry opens next pass");
    auto cells=std::vector<std::uint8_t>(64*64,1);
    for(int y=0;y<64;++y)if(y!=30)cells[y*64+43]=0;
    NavigationGrid narrow(64,64,16,cells);
    const auto pointRoute=narrow.lineWalkable(ally.position,target.position);
    const auto bodyBlocked=run(narrow,136);
    check(pointRoute && bodyBlocked.size()==1 && bodyBlocked[0].source=="focus-fire",
          "point clearance through a narrow opening cannot credit a supporting Zealot body");
}

void rangedSupportRoutingWorkload() {
    constexpr int side=512, cell=8;
    std::vector<std::uint8_t> cells(side*side,1);
    for(int y=0;y<side;++y)cells[y*side+229]=0;
    NavigationGrid grid(side,side,cell,cells);
    auto den=unit(18,UnitKind::hydraliskDen,false,{1904,3808});
    den.hitPoints=den.maxHitPoints=850;den.topSpeed=0;
    den.dimensionLeft=49;den.dimensionRight=48;den.dimensionUp=32;den.dimensionDown=31;
    grid.updateDynamicObstacle(18,{1855,3776,1952,3839});
    const std::array positions{Position{1695,3710},Position{1637,3719},Position{1713,3791},
                              Position{1665,3739},Position{1708,3752},Position{1702,3733}};
    std::vector<UnitSnapshot> melee;
    for(std::size_t i=0;i<positions.size();++i){
        auto ally=unit(static_cast<UnitId>(100+i),UnitKind::zealot,true,positions[i]);
        ally.dimensionLeft=ally.dimensionRight=11;ally.dimensionUp=5;ally.dimensionDown=13;
        ally.groundWeapon={16,22,0,15,DamageType::normal,false,true};melee.push_back(ally);
    }
    std::vector<UnitSnapshot> ranged;
    for(int i=0;i<24;++i){
        auto actor=unit(200+i,UnitKind::dragoon,true,{1480+(i%4)*4,3730+(i/4)*4});
        actor.dimensionLeft=actor.dimensionUp=15;actor.dimensionRight=actor.dimensionDown=16;
        actor.groundWeapon={20,30,0,192,DamageType::explosive,false,true};ranged.push_back(actor);
    }
    CombatEstimate estimate;estimate.decision=FightDecision::engage;estimate.ratio=100;
    InfluenceMap influence;const auto before=NavigationGrid::diagnosticsForCurrentThread();
    const auto start=std::chrono::steady_clock::now();
    TacticalController profiledController;
    const auto commands=profiledController.control(ranged,std::array{den},estimate,den.position,
        {1976,556},influence,{1312,3588},0,false,{},TacticalIntent::battle,melee,&grid,{},
        nullptr,true,{},den.position,22188);
    const auto after=NavigationGrid::diagnosticsForCurrentThread();
    const auto costs=profiledController.lastControlCost();
    check(costs.targetSelections==ranged.size() && costs.supportCalls==144 &&
              costs.supportCacheHits==138 && costs.pathSearches==after.searches-before.searches &&
              costs.expandedNodes==after.totalExpansions-before.totalExpansions,
          "tactical cost telemetry counts real support reuse and navigation work");
    check(costs.totalUs>=costs.targetSelectionUs+costs.contactUs+costs.supportUs,
          "exclusive tactical cost regions fit inside the total measured control pass");
    profiledController.reset();
    check(profiledController.lastControlCost().totalUs==0 &&
              profiledController.lastControlCost().supportCalls==0,
          "reset clears prior tactical profiling state");
    std::cout<<"SUPPORT_WORK,searches="<<after.searches-before.searches
             <<",expanded="<<after.totalExpansions-before.totalExpansions
             <<",ms="<<std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()
             <<",orders="<<commands.size()<<'\n';
    check(after.searches-before.searches<=melee.size() &&
              after.totalExpansions-before.totalExpansions<=melee.size()*2048,
          "ranged units reuse each bounded ally-target support query within a pass");
    check(commands.size()==ranged.size() && std::ranges::all_of(commands,[](const Command& command){
              return command.source=="focus-fire" && command.type==CommandType::attackUnit && command.targetUnit==18;
          }),"inconclusive support queries preserve every ranged attack command");
    std::cout<<"SUPPORT_COMMANDS,";
    for(const auto& command:commands)std::cout<<command.actor<<':'<<command.source<<':'
        <<static_cast<int>(command.type)<<':'<<command.targetUnit<<';';
    std::cout<<'\n';
}

void sharedContactCandidateControls() {
    constexpr int side=10, cell=16;
    std::vector<std::uint8_t> cells(side*side,1);
    for (int y=0;y<side-1;++y) cells[y*side+5]=0;
    NavigationGrid detour(side,side,cell,cells);
    const Position origin{40,24}, across{120,24}, sameSide{40,120};
    std::array goals{NavigationCandidate{across,0},NavigationCandidate{sameSide,150}};
    const auto before=NavigationGrid::diagnosticsForCurrentThread();
    const auto selected=detour.bestReachableCandidate(origin,goals,4096);
    const auto after=NavigationGrid::diagnosticsForCurrentThread();
    check(selected.target==sameSide && selected.status==NavigationStatus::reached &&
              !selected.searchBudgetExhausted && after.searches-before.searches==1,
          "one shared tree respects detour distance and each candidate's crowding penalty");
    goals[1].penalty=500;
    const auto cheaperDetour=detour.bestReachableCandidate(origin,goals,4096);
    check(cheaperDetour.target==across && !cheaperDetour.searchBudgetExhausted &&
              detour.findPath(origin,cheaperDetour.target,4096).reached(),
          "a reachable detour remains available when a direct slot is too crowded");
    const auto denied=detour.bestReachableCandidate(origin,std::array{goals[0]},0);
    const auto partial=detour.bestReachableCandidate(origin,std::array{goals[0]},1);
    check(!denied.target.valid() && denied.searchBudgetExhausted && denied.expandedNodes==0 &&
              !partial.target.valid() && partial.searchBudgetExhausted && partial.expandedNodes==1,
          "denied and incomplete shared searches grant no candidate reachability");
    const auto direct=detour.bestReachableCandidate(origin,std::array{NavigationCandidate{sameSide,0}},0);
    check(direct.target==sameSide && direct.expandedNodes==0 && !direct.searchBudgetExhausted,
          "body-clear straight contact routes need no search allowance");
    const auto knownFallback=detour.bestReachableCandidate(origin,goals,0);
    check(knownFallback.target==sameSide && knownFallback.searchBudgetExhausted,
          "an already proved direct slot can survive an unresolved better alternative");
    auto occupied=detour;
    occupied.updateDynamicObstacle(1,{32,112,48,128});
    const auto blocked=occupied.bestReachableCandidate(origin,goals,0);
    check(!blocked.target.valid(),"dynamic slot occupancy cannot reuse a formerly clear candidate");
    check(!detour.bestReachableCandidate(origin,std::array{NavigationCandidate{{88,24},0}},4096).target.valid(),
          "a blocked endpoint cannot be credited through a nearby snapped goal");
    NavigationGrid corners(2,2,16,{1,0,0,1});
    const auto diagonal=corners.bestReachableCandidate({8,8},std::array{NavigationCandidate{{24,24},0}},4096);
    check(!diagonal.target.valid() && diagonal.status==NavigationStatus::unreachable &&
              !diagonal.searchBudgetExhausted,
          "a fully exhausted shared search rejects a diagonal cut through blocked corners");
    std::vector<std::uint8_t> gapCells(16*16,1);
    for(int y=0;y<16;++y) if(y!=7)gapCells[y*16+8]=0;
    NavigationGrid narrow(16,16,8,gapCells);
    const std::array narrowGoal{NavigationCandidate{{100,60},0}};
    const auto point=narrow.bestReachableCandidate({20,60},narrowGoal,4096);
    const auto body=narrow.bestReachableCandidate({20,60},narrowGoal,4096,{11,11,5,13});
    check(point.target.valid() && !body.target.valid() && !body.searchBudgetExhausted,
          "a point-sized gap never proves an asymmetric Zealot body can pass");
    NavigationGrid open(10,10,16,std::vector<std::uint8_t>(100,1));
    const auto tied=open.bestReachableCandidate({24,24},std::array{
        NavigationCandidate{{24,120},0},NavigationCandidate{{120,24},0}},4096);
    check(tied.target==Position{120,24},"equal contact scores retain deterministic row-column tie breaking");
    check(!open.bestReachableCandidate({24,24},std::array{
              NavigationCandidate{{120,24},-1},
              NavigationCandidate{{24,120},std::numeric_limits<double>::quiet_NaN()}},4096).target.valid(),
          "invalid candidate costs cannot enter contact selection");
}

void candidateWorkspaceReuseControls() {
    // Independent exhaustive Dijkstra oracle: no shared candidate heap/state.
    // Alternate map sizes, footprints and dynamic revisions between queries.
    std::uint32_t random = 0x322c0deU;
    const auto next = [&random] { random = random * 1664525U + 1013904223U; return random; };
    for (int trial = 0; trial < 48; ++trial) {
        const auto side = 12 + trial % 5;
        constexpr int cell = 8;
        std::vector<std::uint8_t> terrain(side * side, 1);
        for (auto& value : terrain) value = next() % 7U != 0U;
        const Position origin{12,12};
        terrain[side+1] = 1;
        NavigationGrid grid(side,side,cell,terrain);
        const MovementFootprint footprint = trial % 2 ? MovementFootprint{2,3,1,2} : MovementFootprint{};
        std::array<NavigationCandidate,5> goals;
        for (std::size_t i=0;i<goals.size();++i) goals[i] = {
            {static_cast<int>(next() % (side-2) + 1)*cell+cell/2,
             static_cast<int>(next() % (side-2) + 1)*cell+cell/2},
            static_cast<double>(i*17 + next()%31U)};
        for (int revision=0;revision<3;++revision) {
            if (revision==1) grid.updateDynamicObstacle(99,{side*4,8,side*4+7,side*8-9});
            if (revision==2) grid.removeDynamicObstacle(99);
            std::vector<double> costs(side*side,std::numeric_limits<double>::infinity());
            std::vector<bool> visited(side*side,false);
            costs[side+1]=0;
            for (int iteration=0;iteration<side*side;++iteration) {
                int current=-1;
                for (int n=0;n<side*side;++n) if (!visited[n] &&
                    (current<0 || costs[n]<costs[current])) current=n;
                if (current<0 || !std::isfinite(costs[current])) break;
                visited[current]=true;
                const auto x=current%side,y=current/side;
                for(int dy=-1;dy<=1;++dy) for(int dx=-1;dx<=1;++dx) {
                    if(dx==0 && dy==0) continue;
                    const auto nx=x+dx,ny=y+dy;
                    if(nx<0 || ny<0 || nx>=side || ny>=side ||
                        !grid.walkable({nx*cell+cell/2,ny*cell+cell/2},footprint)) continue;
                    if(dx!=0 && dy!=0 && (!grid.walkable({nx*cell+cell/2,y*cell+cell/2},footprint) ||
                        !grid.walkable({x*cell+cell/2,ny*cell+cell/2},footprint))) continue;
                    const auto step=(dx!=0 && dy!=0 ? std::sqrt(2.0) : 1.0)*cell;
                    costs[ny*side+nx]=std::min(costs[ny*side+nx],costs[current]+step);
                }
            }
            Position expected{-1,-1}; double score=std::numeric_limits<double>::infinity();
            for(const auto& goal:goals) {
                if(!grid.walkable(goal.position,footprint)) continue;
                const auto route=grid.lineWalkable(origin,goal.position,footprint)
                    ? distance(origin,goal.position)
                    : costs[(goal.position.y/cell)*side+goal.position.x/cell];
                const auto candidateScore=route+goal.penalty;
                if(!std::isfinite(candidateScore)) continue;
                if(candidateScore<score || (candidateScore==score &&
                    (goal.position.y<expected.y || (goal.position.y==expected.y && goal.position.x<expected.x)))) {
                    expected=goal.position;score=candidateScore;
                }
            }
            const auto actual=grid.bestReachableCandidate(origin,goals,4096,footprint);
            check(actual.target==expected && !actual.searchBudgetExhausted &&
                  (!expected.valid() || std::abs(actual.score-score)<1e-8),
                  "reused indexed candidate search matches exhaustive weighted route oracle");
            // Interleave a normal search, which shares the generation and heap.
            (void)grid.findPath(origin,{(side-2)*cell+4,(side-2)*cell+4},24,footprint);
            check(NavigationGrid::diagnosticsForCurrentThread().searchWorkspaceBytes <=
                  NavigationGrid::maximumSearchWorkspaceBytes,
                  "candidate scratch storage stays within the documented per-thread cap");
        }
    }
}

void contactRoutingWorkload() {
    constexpr int side = 512, cell = 8;
    std::vector<std::uint8_t> cells(side * side, 1);
    for (int y = 0; y < side; ++y) cells[y * side + 229] = 0;
    NavigationGrid grid(side, side, cell, cells);
    auto den = unit(18, UnitKind::hydraliskDen, false, {1904,3808});
    den.hitPoints = den.maxHitPoints = 850;
    den.firstSeen = 28388;
    den.dimensionLeft = 49; den.dimensionRight = 48;
    den.dimensionUp = 32; den.dimensionDown = 31;
    den.topSpeed = 0;
    grid.updateDynamicObstacle(18, {1855,3776,1952,3839});
    const std::array positions{Position{1695,3710},Position{1637,3719},Position{1713,3791},
                               Position{1665,3739},Position{1708,3752},Position{1702,3733}};
    const std::array<UnitId,6> actorIds{357,362,381,389,401,402};
    std::vector<UnitSnapshot> actors;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        auto actor = unit(actorIds[i], UnitKind::zealot, true, positions[i]);
        actor.dimensionLeft = actor.dimensionRight = 11;
        actor.dimensionUp = 5; actor.dimensionDown = 13;
        actor.groundWeapon = {16,22,0,15,DamageType::normal,false,true};
        actor.topSpeed = 3.15;
        actors.push_back(actor);
    }
    CombatEstimate estimate; estimate.decision = FightDecision::engage; estimate.ratio = 100;
    InfluenceMap influence;
    const auto before = NavigationGrid::diagnosticsForCurrentThread();
    const auto started = std::chrono::steady_clock::now();
    const auto orders = TacticalController{}.control(actors,std::array{den},estimate,den.position,
        {1976,556},influence,{1312,3588},0,false,{},TacticalIntent::battle,{},&grid,{},
        nullptr,true,{},den.position,28560);
    const auto after = NavigationGrid::diagnosticsForCurrentThread();
    const auto elapsed = std::chrono::duration<double,std::milli>(
        std::chrono::steady_clock::now()-started).count();
    std::cout << "CONTACT_WORK,searches=" << after.searches-before.searches
              << ",expanded=" << after.totalExpansions-before.totalExpansions
              << ",ms=" << elapsed << ",orders=" << orders.size() << '\n';
    check(after.searches-before.searches <= actors.size() &&
              after.totalExpansions-before.totalExpansions <= actors.size()*4096,
          "each contact actor shares one bounded search across its legal slots");
    check(orders.size()==actors.size() && std::ranges::all_of(orders, [](const Command& order) {
              return order.type==CommandType::hold && order.source=="melee-contact-wait";
          }), "unproved contacts across a closed wall remain waiting");
    for (const auto& order : orders)
        std::cout << "CONTACT_ORDER," << order.actor << ',' << order.source << ','
                  << order.targetPosition.x << ',' << order.targetPosition.y << '\n';
}

void zealotContactCoordination() {
    constexpr int width = 40;
    constexpr int height = 30;
    constexpr int cell = 32;
    const auto openCells = std::vector<std::uint8_t>(width * height, 1);
    const NavigationGrid open{width, height, cell, openCells};
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    InfluenceMap influence;
    TacticalController controller;

    auto marine = unit(50, UnitKind::marine, false, {800, 480});
    marine.dimensionLeft = marine.dimensionRight = 16;
    marine.dimensionUp = marine.dimensionDown = 16;
    std::vector<UnitSnapshot> zealots;
    for (auto index = 0; index < 4; ++index) {
        auto zealot = unit(static_cast<UnitId>(10 + index), UnitKind::zealot, true,
                           {600 + index * 20, 480});
        zealot.dimensionLeft = 11;
        zealot.dimensionRight = 11;
        zealot.dimensionUp = 5;
        zealot.dimensionDown = 13;
        zealot.groundWeapon = {16, 22, 0, 32, DamageType::normal, false, true};
        zealots.push_back(zealot);
    }

    auto orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {320, 480}, influence, {400, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &open);
    std::vector<Position> contactSlots;
    auto allContactMoves = orders.size() == zealots.size();
    for (const auto& order : orders) {
        allContactMoves = allContactMoves && order.type == CommandType::move &&
                          order.source == "melee-contact" &&
                          order.targetPosition.valid() && open.walkable(order.targetPosition);
        if (order.source == "melee-contact") contactSlots.push_back(order.targetPosition);
    }
    std::ranges::sort(contactSlots, [](const Position left, const Position right) {
        return left.y != right.y ? left.y < right.y : left.x < right.x;
    });
    auto spacedSlots = contactSlots.size() == zealots.size();
    for (std::size_t first = 0; first < contactSlots.size(); ++first)
        for (std::size_t second = first + 1; second < contactSlots.size(); ++second)
            spacedSlots = spacedSlots &&
                distanceSquared(contactSlots[first], contactSlots[second]) >= 40 * 40;
    check(allContactMoves && spacedSlots,
          "four Zealots receive separate, walkable firing positions in open terrain");

    for (std::size_t index = 0; index < zealots.size() && index < contactSlots.size(); ++index)
        zealots[index].position = contactSlots[index];
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {320, 480}, influence, {400, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &open);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == 50;
          }),
          "all four Zealots issue attacks after reaching their assigned open-terrain slots");

    auto chokeCells = openCells;
    for (auto y = 0; y < height; ++y)
        if (y != 15) chokeCells[static_cast<std::size_t>(y * width + 20)] = 0;
    const NavigationGrid choke{width, height, cell, chokeCells};
    marine.position = {800, 496};
    for (auto& zealot : zealots) zealot.position = {592, 496};
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 496}, influence, {256, 496}, 0, false, {}, TacticalIntent::battle,
        {}, &choke);
    const auto chokeRoutes = orders.size() == zealots.size() &&
        std::ranges::all_of(orders, [&choke](const Command& order) {
            return order.source == "melee-contact" && order.targetPosition.valid() &&
                   choke.findPath({592, 480}, order.targetPosition, 4096).reached();
        });
    check(chokeRoutes,
          "Zealot contact slots behind a one-tile choke remain connected through its opening");
    std::vector<Position> chokeSlots;
    for (const auto& order : orders)
        if (order.source == "melee-contact") chokeSlots.push_back(order.targetPosition);
    for (std::size_t index = 0; index < zealots.size() && index < chokeSlots.size(); ++index)
        zealots[index].position = chokeSlots[index];
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &choke);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == 50;
          }),
          "all four Zealots attack after navigating through the one-tile choke");

    auto blockedCells = openCells;
    for (auto y = 0; y < height; ++y)
        blockedCells[static_cast<std::size_t>(y * width + 20)] = 0;
    const NavigationGrid blocked{width, height, cell, blockedCells};
    for (auto& zealot : zealots) zealot.position = {592, 480};
    orders = controller.control(zealots, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &blocked);
    check(orders.size() == zealots.size() &&
          std::ranges::all_of(orders, [](const Command& order) {
              return order.type == CommandType::hold &&
                     order.source == "melee-contact-wait";
          }),
          "Zealots wait instead of attacking across an impassable wall");

    std::vector<UnitSnapshot> distantArmy;
    for (auto index = 0; index < 64; ++index) {
        auto zealot = zealots.front();
        zealot.id = static_cast<UnitId>(100 + index);
        zealot.position = {256 + index % 4 * 16, 480 + index / 4 * 8};
        distantArmy.push_back(zealot);
    }
    const auto searchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
    orders = controller.control(distantArmy, std::array{marine}, engage, marine.position,
        {224, 480}, influence, {256, 480}, 0, false, {}, TacticalIntent::battle,
        {}, &blocked);
    check(orders.size() == distantArmy.size() &&
          std::ranges::all_of(orders, [target = marine.id](const Command& order) {
              return order.type == CommandType::attackUnit && order.targetUnit == target;
          }) && NavigationGrid::diagnosticsForCurrentThread().searches == searchesBefore,
          "a distant army approaches before running local contact searches for every member");

    auto dragoon = unit(70, UnitKind::dragoon, true, {600, 480});
    dragoon.dimensionLeft = dragoon.dimensionRight = 16;
    dragoon.dimensionUp = dragoon.dimensionDown = 16;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    marine.position = {1024, 480};
    marine.groundWeapon = {};
    auto advancingZealot = zealots.front();
    advancingZealot.position = {850, 480};
    const std::array support{dragoon, advancingZealot};
    orders = controller.control(std::array{dragoon}, std::array{marine}, engage,
        marine.position, {480, 480}, influence, dragoon.position, 0, false, {},
        TacticalIntent::battle, support, &open);
    check(orders.size() == 1 && orders.front().type == CommandType::hold &&
          orders.front().source == "melee-support-hold",
          "an out-of-range Dragoon holds position while its reachable Zealot is still approaching");

    advancingZealot.position = {992, 480};
    const std::array arrivedSupport{dragoon, advancingZealot};
    orders = controller.control(std::array{dragoon}, std::array{marine}, engage,
        marine.position, {480, 480}, influence, dragoon.position, 0, false, {},
        TacticalIntent::battle, arrivedSupport, &open);
    check(orders.size() == 1 && orders.front().type == CommandType::attackUnit &&
          orders.front().targetUnit == marine.id,
          "ranged support resumes its attack order once the Zealot reaches contact");
}

void stormReservationCoordination() {
    auto templar = unit(80, UnitKind::highTemplar, true, {500, 500});
    templar.energy = 150;
    std::vector<UnitSnapshot> enemies;
    for (auto index = 0; index < 4; ++index) {
        auto marine = unit(static_cast<UnitId>(90 + index), UnitKind::marine, false,
                           {650 + index * 12, 500 + (index % 2) * 12});
        marine.hitPoints = marine.maxHitPoints = 40;
        enemies.push_back(marine);
    }
    CombatEstimate engage;
    engage.decision = FightDecision::engage;
    InfluenceMap influence;
    TacticalController controller;
    const auto proposed = controller.control(std::array{templar}, enemies, engage,
        {900, 900}, {100, 100}, influence, templar.position, 6, true);
    const auto firstCast = std::ranges::find_if(proposed, [](const Command& command) {
        return command.type == CommandType::useTech &&
               command.technology == TechnologyKind::psionicStorm;
    });
    check(firstCast != proposed.end(),
          "a safe, valuable Storm candidate is proposed before bridge acceptance");
    if (firstCast == proposed.end()) return;

    auto movedDragoon = unit(81, UnitKind::dragoon, true, firstCast->targetPosition);
    const std::array safeAllies{templar};
    const std::array movedAllies{templar, movedDragoon};
    check(psionicStormSafe(firstCast->targetPosition, enemies, safeAllies) &&
          !psionicStormSafe(firstCast->targetPosition, enemies, movedAllies),
          "live friendly movement into the impact area invalidates the stale cast safety score");

    auto secondTemplar = templar;
    secondTemplar.id = 82;
    const std::array acceptedZone{firstCast->targetPosition};
    const auto rejectedFirstCastRetry = controller.control(
        std::array{secondTemplar}, enemies, engage, {900, 900}, {100, 100}, influence,
        secondTemplar.position, 6, true, {}, TacticalIntent::battle, {}, nullptr, {},
        nullptr, false);
    check(std::ranges::any_of(rejectedFirstCastRetry, [](const Command& command) {
              return command.type == CommandType::useTech &&
                     command.technology == TechnologyKind::psionicStorm;
          }),
          "a rejected proposal leaves no cross-frame reservation and permits reassessment");

    const auto acceptedFirstCast = controller.control(
        std::array{secondTemplar}, enemies, engage, {900, 900}, {100, 100}, influence,
        secondTemplar.position, 6, true, {}, TacticalIntent::battle, {}, nullptr, {},
        nullptr, false, acceptedZone);
    check(std::ranges::none_of(acceptedFirstCast, [](const Command& command) {
              return command.type == CommandType::useTech &&
                     command.technology == TechnologyKind::psionicStorm;
          }),
          "a bridge-accepted Storm reservation suppresses overlapping Templar proposals");
}

void differentialEngineCaseSweeps() {
    const auto decisionRank = [](const FightDecision decision) {
        switch (decision) {
        case FightDecision::retreat: return 0;
        case FightDecision::kite: return 1;
        case FightDecision::engage: return 2;
        }
        return -1;
    };
    const auto favorableMargin = [](const CombatEstimate& estimate) {
        return estimate.simulatedFriendlyRemaining - estimate.simulatedEnemyRemaining;
    };
    CombatEvaluator evaluator;

    // Portable differential replay of the T061 ranged-cooldown lane. These
    // snapshots sweep model inputs around the native matchup archetype; they
    // are not another engine run. BWAPI folds upgrades into weapon damage.
    auto dragoon = unit(1, UnitKind::dragoon, true, {496, 496});
    dragoon.hitPoints = dragoon.maxHitPoints = 100;
    dragoon.maxShields = dragoon.shields = 80;
    dragoon.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
    std::array<decltype(dragoon), 3> zerglings{};
    for (std::size_t index = 0; index < zerglings.size(); ++index) {
        auto& ling = zerglings[index];
        ling = unit(static_cast<UnitId>(10 + index), UnitKind::zergling, false,
                    {640 + static_cast<int>(index) * 12, 496});
        ling.hitPoints = ling.maxHitPoints = 35;
        ling.groundWeapon = {5, 8, 0, 32, DamageType::normal, false, true};
    }

    auto baseline = evaluator.evaluate(std::array{dragoon}, zerglings, 1.2, 0.0);
    auto upgradedDragoon = dragoon;
    for (const auto damage : {22, 24, 26, 28, 30}) {
        upgradedDragoon.groundWeapon.damage = damage;
        const auto upgraded = evaluator.evaluate(std::array{upgradedDragoon}, zerglings, 1.2, 0.0);
        check(upgraded.friendlyPower + 1e-9 >= baseline.friendlyPower &&
              upgraded.ratio + 1e-9 >= baseline.ratio &&
              upgraded.simulatedEnemyRemaining <= baseline.simulatedEnemyRemaining + 1e-9 &&
              upgraded.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(upgraded) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(upgraded.decision) >= decisionRank(baseline.decision),
              "each relevant Dragoon weapon-upgrade step is non-worsening in the T061 ranged case");
        baseline = upgraded;
    }

    // Add effective allied Dragoons from the same engine lane. Each ally can
    // legally damage the ground attackers and shares the captured position and
    // post-upgrade weapon profile, so these are not inert support units.
    std::vector<UnitSnapshot> friendly{dragoon};
    baseline = evaluator.evaluate(friendly, zerglings, 1.2, 0.0);
    for (auto id = UnitId{2}; id <= 5; ++id) {
        auto ally = dragoon;
        ally.id = id;
        ally.position.x += static_cast<int>(id - 1) * 8;
        friendly.push_back(ally);
        const auto reinforced = evaluator.evaluate(friendly, zerglings, 1.2, 0.0);
        check(reinforced.friendlyPower + 1e-9 >= baseline.friendlyPower &&
              reinforced.ratio + 1e-9 >= baseline.ratio &&
              reinforced.simulatedEnemyRemaining <= baseline.simulatedEnemyRemaining + 1e-9 &&
              reinforced.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(reinforced) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(reinforced.decision) >= decisionRank(baseline.decision),
              "adding an effective ally cannot worsen the captured Dragoon-versus-Zergling prediction");
        baseline = reinforced;
    }

    // Removing opponents from the same ranged-cooldown fixture is swept down
    // to one active enemy. The zero-enemy floor intentionally reports the
    // required ratio, so it is outside this ratio-monotonic comparison.
    std::vector<UnitSnapshot> remainingEnemies(zerglings.begin(), zerglings.end());
    baseline = evaluator.evaluate(std::array{dragoon}, remainingEnemies, 1.2, 0.0);
    while (remainingEnemies.size() > 1) {
        remainingEnemies.pop_back();
        const auto fewerEnemies = evaluator.evaluate(std::array{dragoon}, remainingEnemies, 1.2, 0.0);
        check(fewerEnemies.simulatedFriendlyLoss <= baseline.simulatedFriendlyLoss + 1e-9 &&
              fewerEnemies.ratio + 1e-9 >= baseline.ratio &&
              fewerEnemies.simulatedFriendlyRemaining + 1e-9 >= baseline.simulatedFriendlyRemaining &&
              favorableMargin(fewerEnemies) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(fewerEnemies.decision) >= decisionRank(baseline.decision),
              "removing a relevant enemy cannot worsen the captured ranged-case prediction");
        baseline = fewerEnemies;
    }

    // The T061 Zealot/Marine shield lane is also swept over the Marine's
    // observed weapon cooldown. A slower cooldown must not increase modeled
    // friendly losses or make the engagement estimate less favorable.
    auto zealot = unit(50, UnitKind::zealot, true, {496, 496});
    zealot.hitPoints = zealot.maxHitPoints = 100;
    zealot.maxShields = zealot.shields = 60;
    zealot.groundWeapon = {8, 22, 0, 32, DamageType::normal, false, true, 2};
    auto marine = unit(51, UnitKind::marine, false, {560, 496});
    marine.hitPoints = marine.maxHitPoints = 40;
    marine.groundWeapon = {6, 15, 0, 128, DamageType::normal, false, true};
    baseline = evaluator.evaluate(std::array{zealot}, std::array{marine}, 1.2, 0.0);
    for (const auto cooldown : {18, 21, 24, 27, 30}) {
        marine.groundWeapon.cooldown = cooldown;
        const auto slowerEnemy = evaluator.evaluate(std::array{zealot}, std::array{marine}, 1.2, 0.0);
        check(slowerEnemy.simulatedFriendlyLoss <= baseline.simulatedFriendlyLoss + 1e-9 &&
              slowerEnemy.ratio + 1e-9 >= baseline.ratio &&
              favorableMargin(slowerEnemy) + 1e-9 >= favorableMargin(baseline) &&
              decisionRank(slowerEnemy.decision) >= decisionRank(baseline.decision),
              "enemy cooldown sweep does not worsen the T061 shield-and-melee prediction");
        baseline = slowerEnemy;
    }
}
UnitSnapshot screenActor(UnitId id,Position p){UnitSnapshot u;u.id=id;u.kind=UnitKind::dragoon;u.ours=u.visible=u.detected=u.completed=u.powered=true;u.firstSeen=0;u.position=p;u.hitPoints=u.maxHitPoints=100;u.shields=u.maxShields=80;u.topSpeed=4;u.weaponCooldown=20;u.groundWeapon={20,30,0,192,DamageType::explosive,false,true};u.airWeapon={20,30,0,192,DamageType::explosive,true,false};u.dimensionLeft=u.dimensionRight=u.dimensionUp=u.dimensionDown=16;return u;}

void defenseScreenInteriorControls() {

 const Position center{1024,1024},rally{1280,1280};const DefenseArea defense{center,192,{-1,-1},{-1,-1}};CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=2;InfluenceMap influence;TacticalController controller;
 std::vector<UnitSnapshot> group;for(int id=1;id<=32;++id)group.push_back(screenActor(id,center));
 auto run=[&](Frame f){return controller.control(group,{},engage,center,rally,influence,center,0,false,defense,TacticalIntent::battle,{},nullptr,{},nullptr,true,{},center,f);};
 auto commands=run(0);Command selected;bool found=false;double radius=-1;
 for(const auto& c:commands)if(c.source=="defense-screen"){const auto r=distanceSquared(c.targetPosition,center);if(r>radius){radius=r;selected=c;found=true;}}
 check(found,"dense screen assigns at least one forward staging destination");
 check(!found || defense.contains(selected.targetPosition),"the nominal staging destination remains in the defensive area");
 int nearReturn=0,nearStable=0;
 if(found){auto& unit=group[selected.actor-1];const auto goal=selected.targetPosition;const auto dx=goal.x-center.x,dy=goal.y-center.y;const auto length=std::hypot(double(dx),double(dy));
  for(int cycle=0;cycle<3;++cycle){unit.position={goal.x+int(std::lround(dx/length*5)),goal.y+int(std::lround(dy/length*5))};auto next=run(6+cycle*12);
   for(const auto& c:next)if(c.actor==selected.actor){nearReturn+=c.source=="defense-return";nearStable+=c.source=="defense-hold" || (c.source=="defense-screen" && c.targetPosition==goal);}
   unit.position=center;run(12+cycle*12);
  }
 }
 check(!found || (nearReturn==0 && nearStable==3),"small native staging overshoot must not alternate the defender back to the center");
 check(!found || radius<=160.0*160.0,"assigned staging destination leaves at least32pixels of boundary clearance");
 TacticalController lone;auto outside=screenActor(100,{1024,600});auto result=lone.control(std::array{outside},{},engage,center,rally,influence,center,0,false,defense);
 check(result.size()==1 && result[0].source=="defense-return" && result[0].targetPosition==center,"a genuinely escaped healthy defender still returns to the defensive area");
 outside.hitPoints=25;outside.shields=0;result=lone.control(std::array{outside},{},engage,center,rally,influence,center,0,false,defense);
 check(result.size()==1 && result[0].source!="defense-return","a critical wound retains its escape obligation before ordinary return");
 }

void blockedExpansionScreenAnchor() {
    const Position assembly{3708,1138}, expansion{3680,1328};
    const DefenseArea defense{assembly,448,expansion};
    NavigationGrid navigation(512,448,8,std::vector<std::uint8_t>(512*448,1));
    navigation.updateDynamicObstacle(48,{3616,1120,3743,1183});
    navigation.updateDynamicObstacle(232,{3616,1280,3743,1375});
    std::vector<UnitSnapshot> army;
    for(int id=1;id<=9;++id) army.push_back(screenActor(id,{3808+(id%3)*16,892-(id/3)*16}));
    CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=2;
    InfluenceMap influence;TacticalController controller;
    std::vector<TacticalMovementEvidence> evidence;
    auto run=[&](Frame frame,const NavigationGrid* nav) {
        evidence.clear();
        return controller.control(army,{},engage,assembly,{3731,586},influence,assembly,0,false,
            defense,TacticalIntent::battle,{},nav,{},nullptr,true,{},assembly,frame,&evidence);
    };
    const auto commands=run(9696,&navigation);
    check(!navigation.walkable(assembly),"fixture expansion assembly lies inside the neutral geyser");
    check(evidence.size()==army.size() && std::ranges::all_of(evidence,[](const auto& e){return e.screenSlotAssigned;}),
          "blocked expansion assembly still assigns each defender a usable screen slot");
    check(std::ranges::any_of(commands,[](const Command& c){return c.type==CommandType::move && c.targetPosition.y>950;}),
          "uncontested expansion cover advances instead of holding behind its blocked anchor");
    check(std::ranges::all_of(evidence,[&](const auto& e){return !e.screenSlotAssigned ||
        (e.screenSlotInterior && defense.contains(e.assignedScreen) &&
         navigation.walkable(e.assignedScreen,{16,16,16,16}));}),
          "relocated screen retains unit clearance and the current defensive boundary");
    const auto first=evidence;run(9708,&navigation);
    check(first.size()==evidence.size() && std::equal(first.begin(),first.end(),evidence.begin(),
        [](const auto& a,const auto& b){return a.actor==b.actor && a.assignedScreen==b.assignedScreen;}),
          "repeated blocked-anchor observations keep stable screen assignments");
    NavigationGrid sealed(512,448,8,std::vector<std::uint8_t>(512*448,0));
    const auto unavailable=run(9720,&sealed);
    check(std::ranges::all_of(evidence,[](const auto& e){return !e.screenSlotAssigned;}) &&
          std::ranges::all_of(unavailable,[](const Command& c){return c.type==CommandType::hold;}),
          "an entirely blocked screen does not invent an unsafe movement destination");
}

void defenseScreenBoundaryGuards() {
 const Position center{1024,1024}, rally{1280,1280};
 const DefenseArea ring{center,192,{-1,-1},{-1,-1}};
 CombatEstimate engage;engage.decision=FightDecision::engage;engage.ratio=2;
 InfluenceMap influence;std::vector<UnitSnapshot> group;for(int id=1;id<=32;++id)group.push_back(screenActor(id,center));
 TacticalController controller;
 auto run=[&](TacticalController& c, const DefenseArea area, Frame f, std::vector<TacticalMovementEvidence>* evidence=nullptr,
              std::span<const UnitSnapshot> obstacles={}) {
  return c.control(group,{},engage,center,rally,influence,center,0,false,area,TacticalIntent::battle,{},nullptr,obstacles,nullptr,true,{},center,f,evidence);
 };
 auto plain=controller;auto recorded=controller;std::vector<TacticalMovementEvidence> evidence;
 const auto a=run(plain,ring,0);const auto b=run(recorded,ring,0,&evidence);
 check(a.size()==b.size() && std::equal(a.begin(),a.end(),b.begin(),[](const Command& x,const Command& y){return x.actor==y.actor && x.type==y.type && x.targetPosition==y.targetPosition && x.source==y.source;}),
       "boundary telemetry preserves the complete tactical command vector");
 check(evidence.size()==group.size(),"screen and hold proposals are logged independently of acceptance");
 check(std::ranges::all_of(evidence,[](const auto& x){return x.defenseRadius==192 && x.economyCenter==Position{-1,-1} && x.defenseFront==Position{-1,-1} && x.dimensionLeft==16 && x.dimensionRight==16 && x.dimensionUp==16 && x.dimensionDown==16 && std::abs(x.screenClearance-(32+std::hypot(16.0,16.0)))<1e-9;}),
       "boundary proposal retains current ring and body inputs");
 check(std::ranges::all_of(evidence,[](const auto& x){return x.source!="defense-hold" || x.proposedCommand==CommandType::hold;}),
       "hold proposal evidence records its actual command kind");
 check(std::ranges::any_of(evidence,[](const auto& x){return !x.screenSlotAssigned && x.source=="defense-hold";}),
       "a full interior screen records an unassigned hold without inventing a reserved slot");
 controller=recorded;
 auto smaller=ring;smaller.pursuitRadius=112;evidence.clear();run(controller,smaller,24,&evidence);
 check(std::ranges::all_of(evidence,[center](const auto& x){return !x.screenSlotAssigned || distance(center,x.assignedScreen)+32+std::hypot(16.0,16.0)<=112;}),
       "shrinking the current ring releases old exterior screen preferences");
 auto plane=ring;plane.pursuitRadius=320;plane.front={1024,960};evidence.clear();run(controller,plane,48,&evidence);
 check(std::ranges::all_of(evidence,[](const auto& x){return !x.screenSlotAssigned || x.assignedScreen.y-960>=32+std::hypot(16.0,16.0);}),
       "forward terrain plane keeps unit-body and arrival clearance");
 plane.front={1024,1020};evidence.clear();run(controller,plane,72,&evidence);
 check(std::ranges::all_of(evidence,[](const auto& x){return !x.screenSlotAssigned || x.assignedScreen.y-1020>=32+std::hypot(16.0,16.0);}),
       "a moved forward plane invalidates the prior nominally contained slot");
 group[0].dimensionLeft=group[0].dimensionRight=group[0].dimensionUp=group[0].dimensionDown=80;evidence.clear();run(controller,ring,96,&evidence);
 const auto large=std::ranges::find(evidence,1,&TacticalMovementEvidence::actor);
 check(large!=evidence.end() && (!large->screenSlotAssigned || distance(center,large->assignedScreen)+32+std::hypot(80.0,80.0)<=192),
       "large current bodies revalidate retained screen clearance");
 group[0].dimensionLeft=group[0].dimensionRight=group[0].dimensionUp=group[0].dimensionDown=16;
 TacticalController blocked;std::vector<TacticalMovementEvidence> old;evidence.clear();run(blocked,ring,0,&old);
 const auto far=std::ranges::find_if(old,[](const auto& x){return x.screenSlotAssigned && distanceSquared(x.assignedScreen,Position{1024,1024})>=128*128;});
 auto building=screenActor(1000,far==old.end()?Position{1152,1024}:far->assignedScreen);building.kind=UnitKind::pylon;
 run(blocked,ring,6,&evidence,std::array{building});
 check(std::ranges::all_of(evidence,[building](const auto& x){return !x.screenSlotAssigned || std::abs(x.assignedScreen.x-building.position.x)>48 || std::abs(x.assignedScreen.y-building.position.y)>48;}),
       "current building obstruction releases a retained screen slot");
 TacticalController frameless;evidence.clear();run(frameless,ring,-1,&evidence);
 check(std::ranges::all_of(evidence,[center](const auto& x){return !x.screenSlotAssigned || distance(center,x.assignedScreen)+32+std::hypot(16.0,16.0)<=192;}),
       "frameless controller callers also keep interior screen destinations");
 group.clear();for(int id=1;id<=600;++id)group.push_back(screenActor(id,center));TacticalController bounded;evidence.clear();run(bounded,ring,0,&evidence);
 check(evidence.size()==512,"screen proposal evidence remains bounded at512 rows per call");
 check(bounded.screenPreferenceCount()<=TacticalController::maximumScreenPreferences,"dense screen preference storage remains bounded");
 bounded.forgetUnit(1);group={screenActor(1,center)};group[0].firstSeen=100;evidence.clear();run(bounded,ring,100,&evidence);
 check(evidence.size()==1 && evidence[0].firstSeen==100,"forgotten actor proposal evidence uses the new observed lifetime");
}


void defensiveMeleeContactBodyControls() {
    auto makeZealot = [](UnitId id, bool ours, Position p) {
        UnitSnapshot u;
        u.id=id; u.kind=UnitKind::zealot; u.role=UnitRole::groundArmy;
        u.ours=ours; u.position=p; u.completed=u.powered=u.visible=u.detected=true;
        u.hitPoints=u.maxHitPoints=100; u.shields=u.maxShields=60;
        u.firstSeen=id==258?10915:1; u.lastSeen=12396; u.topSpeed=4;
        u.dimensionLeft=u.dimensionRight=11; u.dimensionUp=5; u.dimensionDown=13;
        u.groundWeapon={8,22,0,15,DamageType::normal,false,true}; return u;
    };
    auto bodyInside = [](const UnitSnapshot& u, const DefenseArea& d, Position p) {
        for(auto x:{p.x-u.dimensionLeft,p.x+u.dimensionRight})
            for(auto y:{p.y-u.dimensionUp,p.y+u.dimensionDown})
                if(!d.contains({x,y})) return false;
        return true;
    };
    // This dependent check also lets the unchanged supplement run against the frozen header.
    auto targetMatches = []<class E>(const E& e, const UnitSnapshot& target) {
        if constexpr(requires { e.selectedTarget; e.selectedTargetFirstSeen; e.selectedTargetPosition; })
            return e.selectedTarget==target.id && e.selectedTargetFirstSeen==target.firstSeen &&
                   e.selectedTargetPosition==target.position;
        else return false;
    };
    NavigationGrid nav(512,512,8,std::vector<std::uint8_t>(512*512,1)); InfluenceMap influence;
    auto actor=makeZealot(258,true,{3485,745}); auto target=makeZealot(165,false,{3470,849});
    std::vector<UnitSnapshot> support{actor,makeZealot(150,true,{3470,816})};
    DefenseArea defense{{3552,608},256,{3808,464},{-1,-1}};
    std::vector<UnitSnapshot> obstacles;
    auto run = [&](TacticalController& controller, std::vector<TacticalMovementEvidence>* evidence) {
        CombatEstimate estimate; estimate.decision=FightDecision::engage; estimate.ratio=1.6;
        estimate.holdScreen=defense.active();
        return controller.control(std::array{actor},std::array{target},estimate,{3402,962},{3731,586},
            influence,{3485,679},0,false,defense,TacticalIntent::battle,support,&nav,obstacles,nullptr,true,
            {},{3402,962},12396,evidence);
    };
    auto examine = [&](std::string_view label) {
        TacticalController recorded, silent; std::vector<TacticalMovementEvidence> evidence;
        auto commands=run(recorded,&evidence); const auto without=run(silent,nullptr);
        check(commands==without,"melee proposal telemetry preserves the complete command vector");
        check(commands.size()==1 && (commands[0].source=="melee-contact" || commands[0].source=="melee-contact-wait"),label);
        check(commands.size()==1 && (commands[0].type==CommandType::hold || bodyInside(actor,defense,commands[0].targetPosition)),
              "defensive melee contact keeps every current body corner within radius and front");
        check(evidence.size()==1,"contact move or wait emits one raw movement proposal");
        check(evidence.size()==1 && commands.size()==1 && evidence[0].source==commands[0].source &&
              evidence[0].proposedCommand==commands[0].type && evidence[0].actor==actor.id &&
              evidence[0].firstSeen==actor.firstSeen && evidence[0].frame==12396 &&
              evidence[0].origin==actor.position && evidence[0].destination==
                  (commands[0].type==CommandType::hold?actor.position:commands[0].targetPosition),
              "contact telemetry binds actual command, actor lifetime, origin, and destination");
        check(evidence.size()==1 && targetMatches(evidence[0],target),
              "contact telemetry binds the selected target lifetime and current position");
        check(evidence.size()==1 && evidence[0].defenseCenter==defense.center &&
              evidence[0].defenseRadius==defense.pursuitRadius && evidence[0].defenseFront==defense.front &&
              evidence[0].economyCenter==defense.economyCenter && evidence[0].dimensionLeft==11 &&
              evidence[0].dimensionRight==11 && evidence[0].dimensionUp==5 && evidence[0].dimensionDown==13,
              "contact proposal preserves exact defensive area and asymmetric body dimensions");
    };
    examine("radial boundary uses the real melee contact branch");
    actor.position={3504,740}; target.position={3504,797};
    support={actor,makeZealot(150,true,{3504,764})}; defense.front={3552,800};
    examine("forward plane uses the real melee contact branch");
    // Occupied slots must hold rather than manufacture a clipped, unreachable contact.
    auto blocker=makeZealot(1000,true,target.position); blocker.kind=UnitKind::pylon;
    blocker.dimensionLeft=blocker.dimensionRight=blocker.dimensionUp=blocker.dimensionDown=80;
    blocker.groundWeapon={}; obstacles={blocker}; support={actor}; TacticalController blocked;
    std::vector<TacticalMovementEvidence> waiting; auto held=run(blocked,&waiting);
    check(held.size()==1 && held[0].type==CommandType::hold && held[0].source=="melee-contact-wait",
          "blocked defensive contacts retain the legal wait fallback");
    check(waiting.size()==1 && targetMatches(waiting[0],target) && waiting[0].destination==actor.position,
          "blocked contact wait records the actual selected target without a fictitious move");
    std::vector<TacticalMovementEvidence> capped(512); TacticalController capT, capSilent;
    check(run(capT,&capped)==run(capSilent,nullptr) && capped.size()==512,
          "full proposal evidence remains bounded and cannot change contact commands");
    blocked.forgetUnit(actor.id); actor.firstSeen=20000; target.firstSeen=20001;
    waiting.clear(); run(blocked,&waiting);
    check(waiting.size()==1 && waiting[0].firstSeen==20000 && targetMatches(waiting[0],target),
          "recycled actor and target IDs use their newly observed lifetimes");
    obstacles.clear(); support={actor}; target.position={actor.position.x+20,actor.position.y};
    TacticalController ready; std::vector<TacticalMovementEvidence> readyEvidence; auto attack=run(ready,&readyEvidence);
    check(attack.size()==1 && attack[0].type==CommandType::attackUnit && readyEvidence.empty(),
          "a ready melee attack is not mislabeled as a movement proposal");
    target.position={3470,849}; actor.position={3485,745}; support={actor,makeZealot(150,true,{3470,816})};
    defense={}; TacticalController released; std::vector<TacticalMovementEvidence> freeEvidence;
    auto free=run(released,&freeEvidence);
    check(free.size()==1 && free[0].type==CommandType::move && free[0].source=="melee-contact",
          "unbounded army contact pursuit is preserved");
    check(freeEvidence.size()==1 && targetMatches(freeEvidence[0],target),
          "unbounded contact records its actual selected target too");
    defense={{3552,608},256,{3808,464},{-1,-1}};
    actor.kind=UnitKind::darkTemplar; actor.cloaked=true; TacticalController covert;
    auto covertCommands=run(covert,nullptr);
    check(covertCommands.size()==1 && covertCommands[0].type==CommandType::move &&
          covertCommands[0].source=="melee-contact",
          "undetected covert advance preserves existing pursuit authority");
}


void healthyReturnTerrainPrefixControls() {
    const Position origin{1024,1056}, rally{1024,2048}, home{1024,512};
    const DefenseArea area{rally,192,{2048,256},{-1,-1}};
    const MovementFootprint body{11,11,5,13};
    auto actor=unit(150,UnitKind::zealot,true,origin);
    actor.firstSeen=1; actor.lastSeen=100; actor.weaponCooldown=0;
    actor.hitPoints=actor.maxHitPoints=100; actor.shields=actor.maxShields=60;
    actor.dimensionLeft=actor.dimensionRight=11; actor.dimensionUp=5; actor.dimensionDown=13;
    actor.detected=true; actor.groundWeapon={8,22,0,15,DamageType::normal,false,true};
    auto foe=unit(190,UnitKind::dragoon,false,{1024,1664});
    foe.detected=true; foe.hitPoints=foe.maxHitPoints=100; foe.shields=foe.maxShields=80;
    foe.dimensionLeft=foe.dimensionUp=15; foe.dimensionRight=foe.dimensionDown=16;
    foe.groundWeapon={20,30,0,192,DamageType::explosive,false,true};
    const auto terrain=[] {
        std::vector<std::uint8_t> cells(512*512,1);
        const auto wall=[&](int l,int t,int r,int b){
            for(int y=t/8;y<=b/8;++y)for(int x=l/8;x<=r/8;++x)cells[y*512+x]=0;
        };
        wall(896,768,928,1152); wall(1120,768,1152,1152); wall(896,1120,1152,1152);
        return NavigationGrid{512,512,8,std::move(cells)};
    };
    auto nav=terrain(); InfluenceMap influence; influence.resize(4096,4096);
    GameState state; state.frame=100;state.self.units={actor};state.enemy.units={foe};influence.update(state);
    CombatEstimate estimate;estimate.decision=FightDecision::kite;estimate.ratio=1.2;
    const auto run=[&](TacticalController& controller,const UnitSnapshot& current,
                       const NavigationGrid* grid,DefenseArea defense,Frame frame,int& budget,
                       std::span<const UnitSnapshot> enemies=std::span<const UnitSnapshot>{}) {
        return controller.control(std::array{current},enemies,estimate,rally,home,influence,
            current.position,0,false,defense,TacticalIntent::battle,{},grid,{},nullptr,true,{},
            rally,frame,nullptr,&budget);
    };
    TacticalController controller;int budget=8;
    auto orders=run(controller,actor,&nav,area,100,budget,std::array{foe});
    check(orders.size()==1 && orders[0].source=="defense-return" && orders[0].targetPosition.y<origin.y &&
        nav.lineWalkable(origin,orders[0].targetPosition,body),"blocked healthy return uses a checked full-body terrain prefix");
    check(budget==7 && controller.returnRouteCount()==1,"one return prefix spends one shared search grant");
    const auto waypoint=orders.size()==1?orders[0].targetPosition:Position{-1,-1};
    budget=0; orders=run(controller,actor,&nav,area,112,budget,std::array{foe});
    check(orders.size()==1 && orders[0].targetPosition==waypoint && budget==0,
        "same current context reuses a checked prefix without refilling the search budget");
    auto approaching = actor;
    approaching.position = {waypoint.x, waypoint.y + 32};
    check(nav.lineWalkable(approaching.position, waypoint, body),
          "near-waypoint fixture preserves full-body clearance");
    budget = 0;
    orders = run(controller, approaching, &nav, area, 118, budget, std::array{foe});
    check(orders.size() == 1 && orders[0].targetPosition == waypoint &&
          controller.returnRouteCount() == 1 && budget == 0,
          "a clear waypoint 32 pixels away is retained until reached instead of replanning");
    const auto invalidates=[&](UnitSnapshot current,DefenseArea defense,const NavigationGrid* grid,
                              const char* message) {
        TacticalController fresh;int grant=1;run(fresh,actor,&nav,area,100,grant,std::array{foe});
        grant=0;run(fresh,current,grid,defense,112,grant,std::array{foe});
        check(fresh.returnRouteCount()==0 && grant==0,message);
    };
    auto changed=actor;changed.firstSeen=2;
    invalidates(changed,area,&nav,"a reused unit ID cannot inherit a prior lifetime return prefix");
    changed=actor;changed.kind=UnitKind::dragoon;
    invalidates(changed,area,&nav,"changed unit kind revokes the return prefix");
    for(int side=0;side<4;++side){changed=actor;
        if(side==0)++changed.dimensionLeft;if(side==1)++changed.dimensionRight;
        if(side==2)++changed.dimensionUp;if(side==3)++changed.dimensionDown;
        invalidates(changed,area,&nav,"each asymmetric body dimension invalidates cached clearance");
    }
    for(int field=0;field<4;++field){auto defense=area;
        if(field==0)++defense.center.x;if(field==1)++defense.pursuitRadius;
        if(field==2)++defense.economyCenter.x;if(field==3)defense.front={1024,2300};
        invalidates(actor,defense,&nav,"changed defensive authority cannot reuse an old return prefix");
    }
    auto replaced=terrain();invalidates(actor,area,&replaced,"fresh terrain identity revokes an otherwise identical prefix");
    auto collision=nav;collision.updateDynamicObstacle(91,{1010,980,1040,1008});
    invalidates(actor,area,&collision,"changed collision geometry revokes blocked cached clearance");
    invalidates(actor,area,nullptr,"missing navigation does not authorize an old terrain prefix");
    changed=actor;changed.position=waypoint;
    invalidates(changed,area,&nav,"arrival at a cached waypoint requires fresh route analysis");
    changed=actor;changed.position={1000,800};
    invalidates(changed,area,&nav,"a large observed displacement cannot inherit the old origin route");
    budget=0;run(controller,actor,&nav,area,149,budget,std::array{foe});
    check(controller.returnRouteCount()==0,"return route preferences expire after their bounded observation window");
    budget=1;run(controller,actor,&nav,area,200,budget,std::array{foe});
    budget=0;run(controller,actor,&nav,area,199,budget,std::array{foe});
    check(controller.returnRouteCount()==0,"frame rollback clears return route preferences");
    budget=1;run(controller,actor,&nav,area,210,budget,std::array{foe});controller.forgetUnit(actor.id);
    check(controller.returnRouteCount()==0,"forgotten actor leaves no return route preference");
    budget=1;run(controller,actor,&nav,area,220,budget,std::array{foe});controller.reset();
    check(controller.returnRouteCount()==0,"match reset clears return route preferences");
    budget=1;run(controller,actor,&nav,area,230,budget,std::array{foe});
    auto newWeapon=foe;newWeapon.position=waypoint;budget=0;
    orders=run(controller,actor,&nav,area,242,budget,std::array{newWeapon});
    check(controller.returnRouteCount()==0 && orders.size()==1 && orders[0].source=="combat-retreat" &&
        distance(orders[0].targetPosition,newWeapon.position)>distance(actor.position,newWeapon.position),
        "new current weapon pressure overrides cached safe travel and escapes away");
    TacticalController remote;budget=1;
    run(remote,actor,&nav,area,260,budget,std::array{foe});
    auto remoteWeapon=foe;remoteWeapon.position={1024,700};budget=0;
    orders=run(remote,actor,&nav,area,272,budget,std::array{remoteWeapon});
    bool remoteSafe=orders.size()==1 && orders[0].source=="combat-retreat";
    if(orders.size()==1){const auto target=orders[0].targetPosition;
        const auto count=std::max(1,static_cast<int>(std::ceil(distance(origin,target)/4)));
        for(int i=0;i<=count;++i){auto along=actor;const auto fraction=static_cast<double>(i)/count;
            along.position={origin.x+static_cast<int>(std::lround((target.x-origin.x)*fraction)),
                            origin.y+static_cast<int>(std::lround((target.y-origin.y)*fraction))};
            remoteSafe &= weaponDistance(along,remoteWeapon)>remoteWeapon.groundWeapon.maxRange+32;
        }
    }
    check(remoteSafe && budget==0,
        "a new remote weapon crossing a cached prefix is rechecked without a nearby-threat trigger");
    TacticalController groups;std::vector<UnitSnapshot> actors(12,actor);
    for(std::size_t i=0;i<actors.size();++i)actors[i].id=300+static_cast<int>(i);
    budget=8;
    const auto group=[&](std::span<const UnitSnapshot> members){
        return groups.control(members,std::array{foe},estimate,rally,home,influence,
            origin,0,false,area,TacticalIntent::battle,{},&nav,{},nullptr,true,{},rally,300,nullptr,&budget);
    };
    group(std::span{actors}.first(5));group(std::span{actors}.subspan(5));
    check(budget==0 && groups.returnRouteCount()==8,"multiple squads share eight return queries without a per-squad refill");
    groups.reset();budget=999;group(actors);
    check(budget==0 && groups.returnRouteCount()==8,"oversized external return grants are capped to the callback maximum");
    groups.reset();budget=-7;group(actors);
    check(budget==0 && groups.returnRouteCount()==0,"negative search grants admit no return prefix");
    TacticalController negative;auto outside=area;outside.center={9000,9000};
    budget=1;run(negative,actor,&nav,outside,400,budget);
    check(budget==0 && negative.returnRouteCount()==1,"unusable route analysis is remembered without granting a waypoint");
    budget=1;run(negative,actor,&nav,outside,412,budget);
    check(budget==1,"unchanged failed analysis suppresses repeated route work");
    run(negative,actor,&collision,outside,424,budget);
    check(budget==0,"changed collision context reopens failed analysis using a fresh grant");
    TacticalController bounded;
    for(int i=0;i<520;++i){changed=actor;changed.id=1000+i;budget=1;
        run(bounded,changed,&nav,outside,500,budget);
    }
    check(bounded.returnRouteCount()==TacticalController::maximumReturnRoutes,
        "return preference storage remains bounded under many observed actor lifetimes");
    changed=actor;changed.id=1000;budget=1;run(bounded,changed,&nav,outside,512,budget);
    check(budget==0,"bounded return preferences evict the oldest actor deterministically");
    changed.id=1519;budget=1;run(bounded,changed,&nav,outside,524,budget);
    check(budget==1,"recent return analysis survives bounded preference eviction");
    check(TacticalController::maximumReturnRouteQueries==8 && TacticalController::maximumReturnRouteExpansions==2000,
        "return route work retains its explicit query and expansion bounds");
}

void benchmark() {
    std::vector<UnitSnapshot> friendly, enemy;
    for (int i = 0; i < 96; ++i) {
        auto a = unit(i, UnitKind::dragoon, true, {400 + i % 12 * 20, 400 + i / 12 * 20});
        a.hitPoints = a.maxHitPoints = 100;
        a.groundWeapon = {20, 30, 0, 192, DamageType::normal, false, true};
        a.weaponCooldown = i % 30;
        friendly.push_back(a);
        auto b = a; b.id += 100; b.ours = false; b.kind = UnitKind::zergling;
        b.position.x += 240; b.hitPoints = b.maxHitPoints = 35;
        b.groundWeapon = {5, 8, 0, 32, DamageType::normal, false, true};
        b.weaponCooldown = i % 8;
        enemy.push_back(b);
    }
    std::vector<double> timings;
    double checksum{};
    for (int i = 0; i < 55; ++i) {
        const auto start = std::chrono::steady_clock::now();
        const auto result = CombatEvaluator{}.evaluate(friendly, enemy, 1.1, 0.1);
        const auto end = std::chrono::steady_clock::now();
        checksum += result.ratio;
        if (i >= 5) timings.push_back(std::chrono::duration<double, std::milli>(end - start).count());
    }
    std::ranges::sort(timings);
    std::cout << "96v96 simulation: median_ms=" << timings[25]
              << " max_ms=" << timings.back() << " checksum=" << checksum << '\n';
}
} // namespace

int main(const int argc, char**) {
    if (argc > 3) { rangedSupportRoutingWorkload(); return failures == 0 ? 0 : 1; }
    if (argc > 2) { contactRoutingWorkload(); return failures == 0 ? 0 : 1; }
    if (argc > 1) { benchmark(); return 0; }
    cadence(); boundedApproach(); combatStatusAdmissibility(); splashAndSpellCalibration(); outcomeMetrics(); truncationCoverage(); confidencePolicy(); consumableAttackers(); workerDefense(); unavailableThreats(); terrainSpacing();
    deterministicScheduling(); dragoonFiringCycle(); zealotContactCoordination();
    contactRoutingWorkload();
    sharedContactCandidateControls(); candidateWorkspaceReuseControls();
    rangedSupportRoutingWorkload();
    rangedSupportProofControls();
    stormReservationCoordination();
    defensiveMeleeContactBodyControls();
    defenderEscapePrecedence();
    retreatCorridorSafety();
    healthyReturnTerrainPrefixControls();
    defenseScreenInteriorControls();
    defenseScreenBoundaryGuards();
    blockedExpansionScreenAnchor();
    differentialEngineCaseSweeps();
    if (failures) std::cerr << failures << " combat accuracy checks failed\n";
    else std::cout << checks << " combat accuracy checks passed\n";
    return failures == 0 ? 0 : 1;
}
