#include "protodd/AllInOpening.hpp"
#include "protodd/Squads.hpp"
#include "protodd/Navigation.hpp"
#include <algorithm>
#include <iostream>
#include <fstream>
#include <iterator>
using namespace protodd;
namespace {
int failures{};
void check(bool value, const char* message) {
    if (!value) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
}
UnitSnapshot fighter(UnitId id, UnitKind kind, Position position) {
    UnitSnapshot unit;
    unit.id=id; unit.kind=kind; unit.position=position; unit.ours=true;
    unit.completed=true; unit.visible=true; unit.hitPoints=unit.maxHitPoints=80;
    unit.shields=unit.maxShields=40; unit.role=UnitRole::groundArmy;
    unit.cloaked=kind==UnitKind::darkTemplar;
    unit.groundWeapon={40,30,0,15,DamageType::normal,false,true};
    return unit;
}
GameState world() {
    GameState state; state.frame=8350; state.mapWidthPixels=state.mapHeightPixels=4096;
    state.self.id=0; state.enemy.id=1; state.self.race=Race::protoss; state.enemy.race=Race::terran;
    BaseSnapshot home; home.id=0; home.ownerId=0; home.center={512,512};
    home.mineralLine={480,480}; home.mineralsRemaining=10000;
    BaseSnapshot unknown; unknown.id=1; unknown.center={3200,3200};
    unknown.mineralLine={3264,3200}; unknown.mineralsRemaining=10000;
    unknown.startLocation=true;
    state.bases={home,unknown};
    for (auto kind : {UnitKind::nexus,UnitKind::gateway,UnitKind::gateway,
                     UnitKind::cyberneticsCore,UnitKind::citadelOfAdun,UnitKind::templarArchives}) {
        auto unit=fighter(static_cast<int>(state.self.units.size())+1,kind,home.center);
        unit.role=UnitRole::production; state.self.units.push_back(unit);
    }
    for(int i=0;i<18;++i) state.self.units.push_back(fighter(20+i,UnitKind::probe,home.center));
    return state;
}
StrategicPlan openingPlan(GameState state, std::span<const UnitSnapshot> army) {
    state.self.units.insert(state.self.units.end(),army.begin(),army.end());
    StrategicPlan plan; plan.name="native"; plan.attackTarget={3200,3200};
    plan.rallyPoint={512,512};
    AllInOpeningPlanner opening; opening.reset(AllInBuild::darkTemplar);
    opening.apply(plan,state,{});
    check(opening.phase()==AllInPhase::pressure && plan.posture==Posture::attack,
          "fixture reaches the real Dark Templar pressure phase");
    check(std::ranges::any_of(plan.composition,[](const CompositionTarget& c){
        return c.kind==UnitKind::darkTemplar && c.weight>0;}),"fixture commits Dark Templar production");
    return plan;
}
Squad dtSquad(const std::vector<Squad>& squads) {
    const auto found=std::ranges::find_if(squads,[](const Squad& squad){
        return std::ranges::any_of(squad.units,[](const UnitSnapshot& u){return u.id==100;});});
    check(found!=squads.end(),"completed Dark Templar receives a squad");
    return found!=squads.end()?*found:Squad{};
}
Squad form(const GameState& state, std::span<const UnitSnapshot> army,
           const StrategicPlan& plan, const NavigationGrid* terrain=nullptr) {
    SquadPlanner planner;
    return dtSquad(planner.form(state,army,state.enemy.units,plan,{512,512},terrain));
}
bool settledWithdrawal(GameState state, std::span<const UnitSnapshot> army,
                       const StrategicPlan& plan, const NavigationGrid& terrain) {
    SquadPlanner planner;
    for (int update = 0; update < 2000; ++update, ++state.frame) {
        const auto squad = dtSquad(planner.form(state, army, state.enemy.units,
            plan, {512, 512}, &terrain));
        if (!squad.objectiveRoutePending) return squad.withdrawing;
        if (squad.objective != squad.center || squad.withdrawing) return false;
    }
    return false;
}
}
int main(int argc, char** argv) {
    auto state=world();
    std::vector<UnitSnapshot> solo{fighter(100,UnitKind::darkTemplar,{512,512})};
    auto plan=openingPlan(state,solo);
    auto squad=form(state,solo,plan);
    check(!squad.withdrawing && squad.objective==plan.attackTarget,
          "committed DT probes the attack objective before workers are observed");
    std::vector<UnitSnapshot> mixed{solo.front(),fighter(101,UnitKind::dragoon,{800,512}),
                                   fighter(102,UnitKind::dragoon,{900,512}),
                                   fighter(103,UnitKind::dragoon,{1000,512})};
    auto mixedPlan=openingPlan(state,mixed);
    squad=form(state,mixed,mixedPlan);
    check(squad.role!=SquadRole::baseDefense && !squad.withdrawing && squad.objective==plan.attackTarget,
          "home guard uses supporting fighters before consuming the committed DT");
    auto normal=plan; normal.composition={{UnitKind::dragoon,.8},{UnitKind::darkTemplar,.2}};
    check(form(state,solo,normal).withdrawing,"ordinary DT harassment still waits for an observed safe economy");
    auto assembling=plan; assembling.posture=Posture::hold;
    check(form(state,solo,assembling).withdrawing,"assembly does not grant an offensive probe");
    auto wounded=solo; wounded.front().recentlyDamaged=true;
    check(form(state,wounded,plan).withdrawing,"recently damaged DT does not begin an unobserved probe");
    auto detectedState=state;
    auto turret=fighter(200,UnitKind::missileTurret,plan.attackTarget);
    turret.ours=false; turret.role=UnitRole::detector; turret.sightRange=224;
    turret.groundWeapon={}; detectedState.enemy.units.push_back(turret);
    check(form(detectedState,solo,plan).withdrawing,"known destination detection still rejects a probe");
    detectedState.enemy.units.front().position={1800,1800};
    check(form(detectedState,solo,plan).withdrawing,"known detection across the route still rejects a probe");
    auto scanned=state; scanned.scannerSweeps.push_back(plan.attackTarget);
    check(form(scanned,solo,plan).withdrawing,"active scan prevents a new unobserved probe");
    std::vector<std::uint8_t> cells(128*128,1);
    for(int y=0;y<128;++y)cells[y*128+50]=0;
    NavigationGrid blocked(128,128,32,std::move(cells));
    check(settledWithdrawal(state,solo,plan,blocked),"fully searched unreachable terrain does not grant a probe");
    auto retreating=plan; retreating.posture=Posture::defend;
    check(form(state,solo,retreating).withdrawing,"defensive posture does not grant an offensive probe");
    auto localTarget=plan; localTarget.attackTarget={512,512};
    check(form(state,solo,localTarget).withdrawing,"a home objective does not grant an offensive probe");
    auto workerState=state;
    for(int i=0;i<2;++i) {
        auto worker=fighter(210+i,UnitKind::scv,{3000+i*32,3100});
        worker.ours=false; worker.role=UnitRole::worker; workerState.enemy.units.push_back(worker);
    }
    auto workerRaid=form(workerState,solo,plan);
    check(!workerRaid.withdrawing && workerRaid.objective!=plan.attackTarget,
          "an observed safe worker line keeps priority over an unobserved probe");
    auto breachState=state;
    auto marine=fighter(220,UnitKind::marine,{560,512});marine.ours=false;
    breachState.enemy.units.push_back(marine);
    auto defense=form(breachState,solo,plan);
    check(defense.role==SquadRole::baseDefense && defense.objective!=plan.attackTarget,
          "a real home breach retains defensive assignment");
    auto buildingState = state;
    auto barracks = fighter(230, UnitKind::barracks, plan.attackTarget);
    barracks.ours = false; barracks.role = UnitRole::production;
    barracks.groundWeapon = {};
    barracks.dimensionLeft = 48; barracks.dimensionRight = 47;
    barracks.dimensionUp = 40; barracks.dimensionDown = 39;
    buildingState.enemy.units.push_back(barracks);
    std::vector<std::uint8_t> clearCells(128 * 128, 1);
    NavigationGrid occupied(128, 128, 32, clearCells);
    occupied.updateDynamicObstacle(230, {3152, 3160, 3247, 3239});
    auto approachSquad = form(buildingState, solo, plan, &occupied);
    check(!approachSquad.withdrawing &&
          approachSquad.missionReason == "Probe committed DT attack objective" &&
          occupied.walkable(approachSquad.objective) &&
          approachSquad.objective != plan.attackTarget &&
          distanceSquared(approachSquad.objective, plan.attackTarget) <= 256 * 256,
          "committed DT approaches a known occupied building rather than requiring its blocked center");
    check(form(state, solo, plan, &occupied).withdrawing,
          "unknown blocked terrain cannot grant a building approach");
    check(form(buildingState, solo, normal, &occupied).withdrawing,
          "ordinary mixed-army DT harassment does not gain an objective approach");
    auto approachDetected = buildingState;
    auto approachTurret = turret; approachTurret.position = plan.attackTarget;
    approachDetected.enemy.units.push_back(approachTurret);
    check(form(approachDetected, solo, plan, &occupied).withdrawing,
          "detection at the actual occupied objective rejects a legal approach");
    approachDetected.enemy.units.back().position = {1800, 1800};
    check(form(approachDetected, solo, plan, &occupied).withdrawing,
          "detection across the approach route is still rejected");
    NavigationGrid separated(128, 128, 32, clearCells);
    for (int y = 0; y < 128; ++y)
        separated.updateDynamicObstacle(1000 + y, {1600, y * 32, 1631, y * 32 + 31});
    separated.updateDynamicObstacle(230, {3152, 3160, 3247, 3239});
    check(settledWithdrawal(buildingState, solo, plan, separated),
          "a legal building approach across unreachable terrain is rejected");
    HarassmentRouteBudget diagnostic;
    const auto diagnosed = harassmentObjectiveProbe(buildingState, solo.front(),
        plan.attackTarget, &occupied, &diagnostic);
    check(diagnosed.target.valid() && diagnostic.probeActor == 100 &&
          diagnostic.probeTarget == plan.attackTarget &&
          diagnostic.probeApproach == diagnosed.waypoint &&
          std::string_view(diagnostic.probeReason) == "safe-objective-probe" &&
          diagnostic.probeRouteStatus == NavigationStatus::reached,
          "successful probe diagnostics retain actor, true goal and proved approach");
    diagnostic = {};
    check(!harassmentObjectiveProbe(state, solo.front(), plan.attackTarget,
          &occupied, &diagnostic).target.valid() &&
          std::string_view(diagnostic.probeReason) == "blocked-objective-outside-known-building",
          "blocked unknown objective is distinguished from a route search failure");
    diagnostic = {};
    diagnostic.pathSearches = HarassmentRouteBudget::maximumPathSearches;
    check(!harassmentObjectiveProbe(buildingState, solo.front(), plan.attackTarget,
          &separated, &diagnostic).target.valid() && diagnostic.deferredChecks == 1 &&
          diagnostic.pathSearches == HarassmentRouteBudget::maximumPathSearches &&
          std::string_view(diagnostic.probeReason) == "route-search-count-exhausted",
          "exhausted routing diagnostics preserve the search cap and reject dispatch");
    // Optional sealed native geometry is a development reproduction, not a
    // map template used by the bot or a dependency of the clean test suite.
    if (argc == 2) {
        std::ifstream capture(argv[1], std::ios::binary);
        std::vector<std::uint8_t> nativeCells{std::istreambuf_iterator<char>(capture), {}};
        check(nativeCells.size() == 512U * 448U, "native collision capture has the expected size");
        if (nativeCells.size() == 512U * 448U) {
            NavigationGrid nativeTerrain(512, 448, 8, std::move(nativeCells));
            auto nativeDt = fighter(182, UnitKind::darkTemplar, {3644, 516});
            nativeDt.dimensionLeft = 12; nativeDt.dimensionRight = 11;
            nativeDt.dimensionUp = 6; nativeDt.dimensionDown = 19;
            GameState nativeState;
            nativeState.frame = 8472; nativeState.mapWidthPixels = 4096;
            nativeState.mapHeightPixels = 3584; nativeState.self.id = 0; nativeState.enemy.id = 1;
            const Position nativeGoal{400, 2048};
            auto knownBunker = fighter(700, UnitKind::bunker, nativeGoal);
            knownBunker.ours = false; knownBunker.groundWeapon = {};
            nativeState.enemy.units.push_back(knownBunker);
            auto formationState = world();
            formationState.mapHeightPixels = 3584;
            formationState.bases.front().center = {3808, 464};
            formationState.enemy.units = nativeState.enemy.units;
            auto formationDt = nativeDt; formationDt.id = 100;
            const std::vector<UnitSnapshot> formationArmy{formationDt};
            auto formationPlan = openingPlan(formationState, formationArmy);
            formationPlan.attackTarget = nativeGoal;
            SquadPlanner formationPlanner;
            const auto waiting = dtSquad(formationPlanner.form(formationState, formationArmy,
                formationState.enemy.units, formationPlan, {3808, 464}, &nativeTerrain));
            check(waiting.objectiveRoutePending && !waiting.withdrawing &&
                  waiting.objective == formationDt.position && waiting.objective != nativeGoal,
                  "native route deferral holds a safe committed DT without dispatching or extracting");
            CombatEstimate waitEstimate; waitEstimate.decision = FightDecision::engage;
            waitEstimate.ratio = 3;
            InfluenceMap emptyInfluence;
            emptyInfluence.resize(4096, 3584);
            TacticalController controller;
            const auto waitingOrders = controller.control(formationArmy, {}, waitEstimate,
                waiting.objective, waiting.retreat, emptyInfluence, waiting.center,
                0, false, {}, TacticalIntent::raidRoutePending, {}, &nativeTerrain);
            check(waitingOrders.size() == 1 && waitingOrders.front().type == CommandType::hold &&
                  waitingOrders.front().source == "raid-route-pending",
                  "pending route intent produces a native hold proposal rather than a homeward move");
            auto stormArmy = formationArmy; stormArmy.front().underStorm = true;
            const auto stormOrders = controller.control(stormArmy, {}, waitEstimate,
                waiting.objective, waiting.retreat, emptyInfluence, waiting.center,
                0, false, {}, TacticalIntent::raidRoutePending, {}, &nativeTerrain);
            check(!stormOrders.empty() && stormOrders.front().source == "storm-escape",
                  "Storm escape precedes pending route waiting");
            check(!harassmentObjectiveProbe(nativeState, nativeDt, nativeGoal,
                  &nativeTerrain).target.valid(), "one-shot native route budget rejects the real long route");
            NavigationWorkQueue retained;
            HarassmentOpportunity proved;
            std::size_t peakWork = 0;
            int updates = 0;
            for (; updates < 2000; ++updates) {
                retained.beginFrame(nativeTerrain, nativeState.frame,
                    HarassmentRouteBudget::maximumPathExpansions);
                diagnostic = {};
                proved = harassmentObjectiveProbe(nativeState, nativeDt, nativeGoal,
                    &nativeTerrain, &diagnostic, &retained);
                peakWork = std::max(peakWork, retained.diagnostics().frameWork);
                if (proved.target.valid()) break;
                ++nativeState.frame;
            }
            check(updates > 0 && updates < 2000 && proved.target == nativeGoal &&
                  proved.waypoint == Position{380, 2076} && peakWork <= 2048,
                  "retained probe proves the actual native approach within the unchanged per-update work cap");
            const auto fullRoute = nativeTerrain.findPath(nativeDt.position, proved.waypoint,
                100000, {12, 11, 6, 19});
            check(fullRoute.reached(), "sealed native approach has a complete traversable route");
            if (fullRoute.reached()) {
                auto newDetector = fighter(701, UnitKind::missileTurret,
                    fullRoute.points[fullRoute.points.size() / 2]);
                newDetector.ours = false; newDetector.role = UnitRole::detector;
                newDetector.sightRange = 224; newDetector.groundWeapon = {};
                nativeState.enemy.units.push_back(newDetector);
                ++nativeState.frame;
                retained.beginFrame(nativeTerrain, nativeState.frame, 2048);
                check(!harassmentObjectiveProbe(nativeState, nativeDt, nativeGoal,
                      &nativeTerrain, nullptr, &retained).target.valid(),
                      "a newly observed detector rejects dispatch even with a cached complete native route");
            }
            std::cout << "native retained probe updates=" << updates + 1
                      << " peakWork=" << peakWork << '\n';
        }
    }
    std::cout << "DT squad fixture failures=" << failures << '\n';
    return failures?1:0;
}
