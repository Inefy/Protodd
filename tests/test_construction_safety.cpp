#include "protodd/ConstructionAnchor.hpp"
#include "protodd/BuildTaskProgress.hpp"
#include "protodd/BaseTerrainAnalysis.hpp"
#include "protodd/Navigation.hpp"
#include "protodd/Operations.hpp"
#include "protodd/MacroPlanner.hpp"
#include "protodd/PlacementSafety.hpp"
#include "protodd/PlacementSearch.hpp"
#include "protodd/RouteSafety.hpp"
#include "protodd/UnitCatalog.hpp"

#include <array>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

int main() {
    using namespace protodd;
    int failures = 0;
    const auto check = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << message << '\n'; }
    };

    // Native r303: the second Cannon at (1280,416) died while warping beyond
    // the first Cannon at (1088,160). The winning pair was only96pixels apart.
    {
        const Position base{1056, 272};
        std::array cannons{CannonConstructionSupport{
            165, {1088, 160}, 224, true, true, false, 195}};
        const auto select = [&](UnitKind kind = UnitKind::photonCannon,
                                Frame frame = 3142, int nexuses = 1,
                                bool explicitSite = false) {
            return selectOpeningCannonPairSupport(
                kind, frame, nexuses, base, explicitSite, 750, cannons);
        };
        const auto projected = select();
        check(projected.active() && projected.id == 165,
              "a paid powered opening Cannon that finishes first supplies projected pair cover");
        check(!openingCannonPlacementAllowed({1280, 416}, projected),
              "the recorded exposed second Cannon must not pass opening pair admission");
        check(openingCannonPlacementAllowed({1184, 160}, projected) &&
                  openingCannonPlacementAllowed({1312, 160}, projected) &&
                  !openingCannonPlacementAllowed({1313, 160}, projected),
              "close supporting geometry and the exact firing boundary remain available");
        auto winningFirst = cannons.front();
        winningFirst.center = {1056, 160};
        const std::array winningPair{winningFirst};
        check(openingCannonPlacementAllowed({1152, 160},
                  selectOpeningCannonPairSupport(UnitKind::photonCannon, 2892, 1,
                      base, false, 750, winningPair)),
              "the recorded successful pair retains supporting placement geometry");
        check(!openingCannonPlacementAllowed({-1, -1}, projected),
              "an invalid Cannon candidate never supplies supporting geometry");
        const BuildingFootprint firstFootprint{{1056, 128}, 64, 64};
        const BuildingFootprint adjacentPair{{1120, 128}, 64, 64};
        const auto pairGap = openingCannonStructureGap(UnitKind::photonCannon,
            UnitKind::photonCannon, projected.center, 32, projected);
        check(!separatedByGap(firstFootprint, adjacentPair, 32) && pairGap == 0 &&
                  separatedByGap(firstFootprint, adjacentPair, pairGap) &&
                  openingCannonPlacementAllowed({1152, 160}, projected),
              "adjacent supporting Cannons are not excluded by the generic structure gap");
        check(!separatedByGap(firstFootprint, {{1088, 128}, 64, 64}, pairGap),
              "the pair spacing exception never admits overlapping build footprints");
        check(openingCannonStructureGap(UnitKind::photonCannon, UnitKind::gateway,
                  projected.center, 32, projected) == 32 &&
                  openingCannonStructureGap(UnitKind::gateway, UnitKind::photonCannon,
                  projected.center, 32, projected) == 32 &&
                  openingCannonStructureGap(UnitKind::photonCannon, UnitKind::photonCannon,
                  {1056, 160}, 32, projected) == 32 &&
                  openingCannonStructureGap(UnitKind::photonCannon, UnitKind::photonCannon,
                  projected.center, 32, {}) == 32,
              "producer, unrelated Cannon and ordinary construction spacing stays mandatory");
        cannons.front().completed = true;
        cannons.front().remainingBuild = -1;
        check(select().active(), "an operational completed first Cannon supports its opening pair");
        for (int unavailable = 0; unavailable < 4; ++unavailable) {
            auto impaired = cannons;
            if (unavailable == 0) impaired.front().available = false;
            if (unavailable == 1) impaired.front().powered = false;
            if (unavailable == 2) impaired.front().center = {-1, -1};
            if (unavailable == 3) impaired.front().groundRange = 0;
            check(!selectOpeningCannonPairSupport(UnitKind::photonCannon, 3142, 1,
                      base, false, 750, impaired).active(),
                  "unavailable or non-firing Cannons cannot manufacture opening support");
        }
        cannons.front().completed = false;
        for (const auto remaining : {-1, 751}) {
            cannons.front().remainingBuild = remaining;
            check(!select().active(),
                  "unproved or later first-Cannon completion cannot be credited as projected cover");
        }
        cannons.front().remainingBuild = 195;
        check(!select(UnitKind::shieldBattery).active() &&
                  !select(UnitKind::photonCannon, 8 * 60 * 24).active() &&
                  !select(UnitKind::photonCannon, 3142, 2).active() &&
                  !select(UnitKind::photonCannon, 3142, 1, true).active(),
              "batteries, mature defenses, expanded economies and explicit sites retain normal placement");
        check(!selectOpeningCannonPairSupport(UnitKind::photonCannon, 3142, 1,
                  base, false, 750, {}).active(),
              "the first Cannon needs no nonexistent partner");
        const std::array multiple{cannons.front(), cannons.front()};
        check(!selectOpeningCannonPairSupport(UnitKind::photonCannon, 3142, 1,
                  base, false, 750, multiple).active(),
              "later Cannon additions are not restricted by the first-pair rule");
    }

    // API acceptance at 2926 did not replace the Probe's AttackUnit order in
    // the native trace. Recover after latency instead of leasing that chase
    // until the route deadline hundreds of frames later.
    check(!constructionBuildOrderLost(2938, 2926, 6, false) &&
              constructionBuildOrderLost(2939, 2926, 6, false) &&
              constructionBuildOrderLost(3548, 2926, 6, false),
          "an unexecuted Build gets a latency grace and then prompt lease recovery");
    check(!constructionBuildOrderLost(3548, 2926, 6, true) &&
              !constructionBuildOrderLost(3548, -1, 6, false) &&
              !constructionBuildOrderLost(2000, 2926, 6, false),
          "active native construction, staged movement, and frame rewinds are not lost Build orders");
    check(!constructionBuildOrderLost(2956, 2926, 24, false) &&
              constructionBuildOrderLost(2957, 2926, 24, false),
          "build recovery respects longer configured command latency");

    // Fallback validation closes over the live search entry. It must survive
    // both validations and remain available while a producer-exit proof waits.
    {
        struct Search { int validated{}; Position pending{-1, -1}; };
        std::unordered_map<int, Search> searches;
        auto& continuation = searches[7];
        const std::array candidates{Position{12, 10}, Position{14, 10}};
        auto ready = false;
        const auto validate = [&](Position tile) {
            check(searches.contains(7), "placement search retired before fallback validation");
            if (!searches.contains(7)) return PlacementFallbackStatus::rejected;
            ++continuation.validated;
            if (tile == candidates[0]) return PlacementFallbackStatus::rejected;
            continuation.pending = tile;
            return ready ? PlacementFallbackStatus::accepted : PlacementFallbackStatus::deferred;
        };
        const auto retire = [&] { searches.erase(7); };
        const auto deferred = finishPlacementFallbacks(candidates, validate, retire);
        check(deferred.status == PlacementFallbackStatus::deferred && !deferred.tile.valid() &&
                  searches.contains(7) && continuation.pending == candidates[1],
              "unfinished fallback exit proof lost its continuation or admitted an unproved tile");
        ready = true;
        const auto resumed = finishPlacementFallbacks(candidates, validate, retire);
        check(resumed.status == PlacementFallbackStatus::accepted &&
                  resumed.tile == candidates[1] && searches.empty(),
              "fallback did not resume safely and retire after its validation");
        auto checks = 0;
        auto retired = false;
        const auto rejected = finishPlacementFallbacks(candidates,
            [&](Position) { ++checks; return PlacementFallbackStatus::rejected; },
            [&] { retired = true; });
        check(rejected.status == PlacementFallbackStatus::rejected && !rejected.tile.valid() &&
                  checks == 2 && retired,
              "unreachable or occupied fallbacks escaped final validation");
    }

    // R57: prepare a requested expansion before its construction bank is full.
    {
        GameState state;
        state.frame = 8308;
        state.self.id = 1;
        state.self.minerals = 275;
        state.self.supplyTotal = 80;
        state.self.supplyUsed = 44;
        state.estimatedMineralIncomePerMinute = 1200;
        UnitSnapshot nexus;
        nexus.id = 1; nexus.kind = UnitKind::nexus; nexus.completed = true;
        nexus.ours = true; nexus.position = {3808,464}; nexus.role = UnitRole::resourceDepot;
        UnitSnapshot probe;
        probe.id = 139; probe.kind = UnitKind::probe; probe.completed = true;
        probe.ours = true; probe.position = {3956,592}; probe.role = UnitRole::worker;
        UnitSnapshot pylon;
        pylon.id = 2; pylon.kind = UnitKind::pylon; pylon.completed = true;
        pylon.ours = true; pylon.position = {3760,320}; pylon.role = UnitRole::supply;
        state.self.units = {nexus,probe,pylon};
        StrategicPlan plan;
        plan.desiredBases = 2; plan.desiredWorkers = 1;
        plan.expansionTarget = {3680,1328};
        ProductionGoal goal{GoalKind::expand,UnitKind::nexus,2,120,true,"prepare replacement income"};
        goal.constructionSite.id = 15805479650608ULL;
        goal.constructionSite.anchor = {3680,1328};
        plan.goals = {goal};
        const auto evaluate = [](const GameState& current, const StrategicPlan& strategy,
                                 ResourceLedger& ledger) {
            MacroPlanner planner;
            return planner.reconcile(current,strategy,ledger);
        };
        ResourceLedger ledger{275,0};
        const auto actions = evaluate(state,plan,ledger);
        const auto expansion = std::ranges::find(actions,UnitKind::nexus,&MacroAction::target);
        check(expansion != actions.end() && !expansion->reserved &&
                  expansion->preparationFundingFrames == 200 && ledger.committedMinerals == 0,
              "observed income permits bounded travel preparation without construction spending authority");
        state.estimatedMineralIncomePerMinute = -1;
        ResourceLedger unknownLedger{275,0};
        const auto unknown = evaluate(state,plan,unknownLedger);
        check(std::ranges::all_of(unknown,[](const MacroAction& action) {
                  return action.preparationFundingFrames < 0;
              }),"unknown income cannot promise funding for an early expansion builder");
        state.estimatedMineralIncomePerMinute = 0;
        ResourceLedger zeroLedger{275,0};
        const auto zero = evaluate(state,plan,zeroLedger);
        check(std::ranges::all_of(zero,[](const MacroAction& action) {
                  return action.preparationFundingFrames < 0;
              }),"collapsed income cannot initiate an unfunded preparation lease");
        state.estimatedMineralIncomePerMinute = 1200;
        plan.goals.insert(plan.goals.begin(),
            ProductionGoal{GoalKind::build,UnitKind::pylon,2,130,true,"urgent supply"});
        ResourceLedger higherLedger{275,0};
        const auto higher = evaluate(state,plan,higherLedger);
        const auto afterSupply = std::ranges::find(higher,UnitKind::nexus,&MacroAction::target);
        check(afterSupply != higher.end() && afterSupply->preparationFundingFrames == 360 &&
                  !afterSupply->reserved && higherLedger.committedMinerals == 100,
              "preparation funding excludes money reserved for an earlier priority obligation");
        check(expansionPreparationFundingWaitFrames(0,800)==-1 &&
                  expansionPreparationFundingWaitFrames(25,1000)==720 &&
                  expansionPreparationFundingWaitFrames(24,1000)==-1,
              "the conservative thirty-second preparation forecast admits its boundary and rejects longer waits");
        plan.deferExpansion = true;
        ResourceLedger deferredLedger{275,0};
        const auto deferred = evaluate(state,plan,deferredLedger);
        check(std::ranges::all_of(deferred,[](const MacroAction& action) {
                  return action.preparationFundingFrames < 0;
              }),"explicit expansion deferral removes early travel preparation permission");
    }

    // R56: replay the observed R55 Probe 142 loop with its unchanged Move target.
    {
        struct Sample { Frame frame; Position position; };
        const std::array samples{
        Sample{9956, {3489, 934}},
        Sample{9980, {3592, 915}},
        Sample{10004, {3709, 916}},
        Sample{10028, {3806, 951}},
        Sample{10052, {3846, 964}},
        Sample{10076, {3753, 927}},
        Sample{10100, {3642, 916}},
        Sample{10124, {3524, 916}},
        Sample{10148, {3421, 885}},
        Sample{10172, {3352, 904}},
        Sample{10196, {3435, 886}},
        Sample{10220, {3539, 916}},
        Sample{10244, {3655, 916}},
        Sample{10268, {3763, 932}},
        Sample{10292, {3799, 939}},
        Sample{10316, {3692, 916}},
        Sample{10340, {3573, 916}},
        Sample{10364, {3470, 886}},
        Sample{10388, {3351, 886}},
        Sample{10412, {3387, 888}},
        Sample{10436, {3501, 900}},
        Sample{10460, {3608, 916}},
        Sample{10484, {3724, 916}},
        Sample{10508, {3811, 956}},
        Sample{10532, {3737, 919}},
        Sample{10556, {3623, 916}},
        Sample{10580, {3504, 916}}
        };
        const Position waypoint{3532, 1124};
        BuildWaypointProgress progress;
        bool reconsidered = false;
        for (const auto& sample : samples) {
            recordBuildWaypointProgress(progress, sample.frame, sample.position, waypoint);
            if (!constructionWaypointMakingProgress(
                    progress, sample.frame, sample.position, true, true, waypoint))
                reconsidered = true;
        }
        check(reconsidered,
              "the observed moving Probe loop must stop retaining its unchanged waypoint");
        BuildWaypointProgress steady;
        for (int step = 0; step < 8; ++step) {
            const Position current{80 + step * 24, 80};
            recordBuildWaypointProgress(steady, step * 40, current, {800, 80});
            check(constructionWaypointMakingProgress(
                      steady, step * 40, current, true, true, {800, 80}),
                  "meaningful progress retains the active accepted route leg");
        }
        check(!constructionWaypointMakingProgress(
                  steady, 329, {248,80}, true, true, {800,80}),
              "motion without a new best waypoint distance cannot refresh retention");
        check(!constructionWaypointMakingProgress(
                  steady, 280, {248,80}, true, true, {900,80}) &&
              !constructionWaypointMakingProgress(
                  steady, 280, {248,80}, true, false, {800,80}),
              "a different command target or lost native Move cannot retain the leg");
        check(!constructionWaypointMakingProgress(
                  steady, 280, {720,80}, true, true, {800,80}),
              "arrival inside the handoff radius releases waypoint retention");
        recordBuildWaypointProgress(steady, 330, {248,80}, {280,800});
        check(steady.waypoint == Position{280,800} && steady.lastProgress == 330,
              "a different accepted waypoint starts an independent progress baseline");
        recordBuildWaypointProgress(steady, 331, {248,80}, {-1,-1});
        check(!constructionWaypointMakingProgress(
                  steady, 331, {248,80}, true, true, {280,800}),
              "clearing the accepted waypoint clears its retained progress");
    }

    // A far-side destination is reachable around the end of this long wall,
    // but not within the normal per-query expansion allowance. Refusing a
    // partial route remains correct; describing it as unreachable does not.
    std::vector<std::uint8_t> longCells(600 * 300, 1U);
    for (int y = 0; y < 299; ++y) longCells[y * 600 + 300] = 0U;
    NavigationGrid longRoute{600, 300, 8, std::move(longCells)};
    const Position longFrom{80, 80}, longTo{4600, 400};
    auto boundedStatus = NavigationStatus::invalidInput;
    auto boundedExhausted = false;
    check(routePath(longFrom, longTo, false, &longRoute, {}, &boundedStatus, &boundedExhausted).empty() &&
              boundedExhausted &&
              (boundedStatus == NavigationStatus::partial ||
               boundedStatus == NavigationStatus::budgetExhausted),
          "a refused bounded route retains unknown reachability instead of claiming a blockade");
    const auto unknownRoute = assessRouteThreat(InfluenceMap{}, longFrom, longTo,
                                               false, &longRoute);
    check(!unknownRoute.reachable && !std::isfinite(unknownRoute.score()) &&
              unknownRoute.navigationStatus == boundedStatus && unknownRoute.searchBudgetExhausted,
          "risk screening retains the bounded path status without using an unsafe prefix");
    const auto unknownAlternative = selectSafeRouteAlternative(
        InfluenceMap{}, longFrom, longTo, false, &longRoute);
    check(!unknownAlternative.profile.reachable &&
              unknownAlternative.profile.navigationStatus == boundedStatus &&
              unknownAlternative.profile.searchBudgetExhausted,
          "construction route selection preserves unfinished baseline search knowledge");
    const auto nearGoalBudgetPrefix = longRoute.findPath(longFrom, {2450,80}, 12000);
    check(nearGoalBudgetPrefix.status == NavigationStatus::partial &&
              nearGoalBudgetPrefix.searchBudgetExhausted,
          "a bounded prefix within the goal snap radius still reports its exhausted search budget");
    const auto completeLongRoute = longRoute.findPath(longFrom, longTo, 400000);
    check(completeLongRoute.reached(),
          "the bounded-route fixture is actually reachable with sufficient search work");
    BaseTerrainAnalysis baseTerrain;
    const std::array terrainSites{longTo, Position{160,80}, Position{240,80}};
    baseTerrain.initialize(longFrom, terrainSites, {});
    check(baseTerrain.advance(longRoute, 0).empty() && baseTerrain.lastWork() == 0 &&
              baseTerrain.remaining() == terrainSites.size(),
          "zero terrain work does not publish unfinished distance knowledge or discard jobs");
    check(baseTerrain.advance(longRoute, 257).empty() && baseTerrain.lastWork() <= 257 &&
              baseTerrain.remaining() == terrainSites.size(),
          "a long home route remains pending across the bounded observation allowance");
    baseTerrain.scheduleEnemy(longTo);
    const auto withEnemy = baseTerrain.remaining();
    baseTerrain.scheduleEnemy(longTo);
    baseTerrain.scheduleEnemy({-1,-1});
    check(withEnemy == 2 * terrainSites.size() && baseTerrain.remaining() == withEnemy,
          "legally scheduled enemy routes are retained exactly once alongside unfinished home routes");
    std::vector<BaseTerrainUpdate> terrainUpdates;
    for (auto call = 0; call < 15000 && baseTerrain.remaining() > 0; ++call) {
        const auto batch = baseTerrain.advance(longRoute, 257);
        check(baseTerrain.lastWork() <= 257,
              "all home/enemy search and length work shares the callback allowance");
        terrainUpdates.insert(terrainUpdates.end(), batch.begin(), batch.end());
    }
    double expectedLength{};
    for (std::size_t i = 1; i < completeLongRoute.points.size(); ++i)
        expectedLength += distance(completeLongRoute.points[i-1], completeLongRoute.points[i]);
    check(baseTerrain.remaining() == 0 && terrainUpdates.size() == withEnemy &&
              terrainUpdates.front().work == BaseTerrainWork::homeDistance &&
              terrainUpdates.front().site == 0 && terrainUpdates.front().reachabilityKnown &&
              terrainUpdates.front().groundDistance == static_cast<int>(std::lround(expectedLength)),
          "retained long terrain work eventually publishes every exact distance without a bounded-search cutoff");
    std::vector<std::uint8_t> separatedCells(20*10, 1U);
    for (auto y = 0; y < 10; ++y) separatedCells[y*20+10] = 0U;
    NavigationGrid separatedTerrain{20,10,32,std::move(separatedCells)};
    const std::array disconnectedSites{Position{560,144}};
    baseTerrain.initialize({80,144}, disconnectedSites, {});
    check(baseTerrain.advance(separatedTerrain, 1).empty(),
          "an unfinished disconnected search does not announce global unreachability");
    std::vector<BaseTerrainUpdate> disconnectedUpdates;
    for (auto call = 0; call < 1000 && baseTerrain.remaining() > 0; ++call) {
        const auto batch = baseTerrain.advance(separatedTerrain, 5);
        check(baseTerrain.lastWork() <= 5, "disconnected terrain work stays bounded too");
        disconnectedUpdates.insert(disconnectedUpdates.end(), batch.begin(), batch.end());
    }
    check(disconnectedUpdates.size() == 1 && disconnectedUpdates[0].reachabilityKnown &&
              disconnectedUpdates[0].groundDistance == -1,
          "only a completely exhausted component publishes known-unreachable terrain");
    std::vector<std::uint8_t> chokeCells(64*40, 1U), chokeHeights(64*40, 0U);
    for (auto y = 0; y < 40; ++y) for (auto x = 0; x < 64; ++x) {
        if (x >= 30 && x <= 32 && (y < 18 || y > 21)) chokeCells[y*64+x] = 0U;
        if (x < 32) chokeHeights[y*64+x] = 2U;
    }
    NavigationGrid terrainChoke{64,40,32,std::move(chokeCells),std::move(chokeHeights)};
    const Position chokeHome{256,640}, chokeOutside{1664,640};
    const std::array chokeSites{chokeHome}, chokeApproaches{chokeOutside};
    const auto expectedDefense = terrainChoke.defensivePosition(chokeHome, chokeOutside);
    baseTerrain.initialize(chokeHome, chokeSites, chokeApproaches);
    std::vector<BaseTerrainUpdate> chokeUpdates;
    auto defenseCalls = 0;
    while (baseTerrain.remaining() > 0 && ++defenseCalls < 1000) {
        const auto batch = baseTerrain.advance(terrainChoke, 257);
        check(baseTerrain.lastWork() <= 257,
              "defensive cross-section and route work stays within the same observation allowance");
        chokeUpdates.insert(chokeUpdates.end(), batch.begin(), batch.end());
    }
    check(defenseCalls > 1 && chokeUpdates.size() == 2 && expectedDefense.valid() &&
              chokeUpdates.back().defense.anchor == expectedDefense.anchor &&
              chokeUpdates.back().defense.entrance == expectedDefense.entrance &&
              chokeUpdates.back().defense.width == expectedDefense.width &&
              chokeUpdates.back().defense.highGround == expectedDefense.highGround,
          "resumed defensive scoring retains the exact choke and upper-edge choice");
    baseTerrain.reset();
    check(baseTerrain.remaining() == 0 && baseTerrain.lastWork() == 0 &&
              baseTerrain.advance(terrainChoke).empty(),
          "match reset releases pending terrain jobs and publishes no prior-map results");
    NavigationSearch continuedRoute;
    auto zeroWork = continuedRoute.advance(longRoute, longFrom, longTo, 0);
    check(zeroWork.status == NavigationStatus::budgetExhausted && zeroWork.points.empty() &&
              continuedRoute.diagnostics().stepWork == 0,
          "zero work leaves exact-route knowledge pending without publishing a prefix");
    auto resumedCalls = 0;
    auto reconstructionPendingCalls = 0;
    auto resumedReached = false;
    while (++resumedCalls < 2000) {
        const auto precedingExpansions = continuedRoute.diagnostics().totalExpansions;
        const auto& result = continuedRoute.advance(longRoute, longFrom, longTo, 257);
        check(continuedRoute.diagnostics().stepWork <= 257,
              "each continued route step respects its total search/reconstruction work allowance");
        if (result.searchBudgetExhausted) {
            if (continuedRoute.diagnostics().totalExpansions == precedingExpansions &&
                precedingExpansions > 0) ++reconstructionPendingCalls;
            check(result.status == NavigationStatus::budgetExhausted && result.points.empty(),
                  "an unfinished exact-route session cannot leak an executable partial path");
            // Ordinary navigation owns different scratch, even on this thread.
            if (resumedCalls == 3)
                check(longRoute.findPath(longFrom, {160,80}).reached(),
                      "ordinary scratch can be reused while an owned long search remains pending");
            continue;
        }
        resumedReached = result.reached();
        check(result.points == completeLongRoute.points,
              "resuming a long detour preserves the ordinary search's deterministic complete route");
        for (std::size_t i = 1; i < result.points.size(); ++i)
            check(longRoute.lineWalkable(result.points[i-1], result.points[i]),
                  "every published resumed route leg is collision clear");
        break;
    }
    check(resumedReached && resumedCalls > 46 && continuedRoute.diagnostics().starts == 1 &&
              continuedRoute.diagnostics().totalExpansions > 12000 &&
              reconstructionPendingCalls > 0 &&
              continuedRoute.diagnostics().workspaceBytes <= NavigationGrid::maximumSearchWorkspaceBytes,
          "a reachable route exceeding the old quota completes without restarting or exceeding bounded scratch");
    check(continuedRoute.diagnostics().outputBytes <= NavigationSearch::maximumOutputBytes,
          "completed exact routes retain a bounded separate output allocation");
    const auto expansionsAtCompletion = continuedRoute.diagnostics().totalExpansions;
    check(continuedRoute.advance(longRoute, longFrom, longTo, 1).reached() &&
              continuedRoute.diagnostics().stepWork == 0 &&
              continuedRoute.diagnostics().totalExpansions == expansionsAtCompletion,
          "a settled unchanged exact-route query performs no additional search work");
    NavigationWorkQueue sharedConstructionWork;
    sharedConstructionWork.beginFrame(longRoute,0,257);
    check(sharedConstructionWork.query(1,longRoute,longFrom,longTo).searchBudgetExhausted &&
              sharedConstructionWork.query(2,longRoute,{96,80},{4600,408}).searchBudgetExhausted,
          "interleaved construction actors share one allowance instead of each running an independent full search");
    auto firstQueuedFinished = false, secondQueuedFinished = false;
    for (auto frame = 1; frame < 2000; ++frame) {
        sharedConstructionWork.beginFrame(longRoute,frame,257);
        const Position miningPosition{80 + (frame % 2)*8,80};
        const auto first = sharedConstructionWork.query(1,longRoute,miningPosition,longTo);
        const auto second = sharedConstructionWork.query(2,longRoute,{96,80},{4600,408});
        if (first.reached()) {
            firstQueuedFinished = true;
            check(first.points.front() == miningPosition && first.points.back() == longTo,
                  "a moving miner joins the retained completed route from its current position");
            for (std::size_t i = 1; i < first.points.size(); ++i)
                check(longRoute.lineWalkable(first.points[i-1],first.points[i]),
                      "a current-position join preserves a fully collision-clear route");
        }
        secondQueuedFinished = secondQueuedFinished || second.reached();
        check(sharedConstructionWork.diagnostics().frameWork <= 257,
              "all actors and current-position joins respect one aggregate frame allowance");
        if (firstQueuedFinished && secondQueuedFinished) break;
    }
    check(firstQueuedFinished && secondQueuedFinished &&
              sharedConstructionWork.diagnostics().invalidated == 0 &&
              sharedConstructionWork.diagnostics().completed == 2,
          "FIFO construction work settles both actors without miner-motion restarts or candidate starvation");

    // A selected expansion candidate must follow accepted leases without
    // cancelling the current frontier or starving the other queued actors.
    {
        NavigationGrid priorityGrid(160, 100, 8,
            std::vector<std::uint8_t>(160 * 100, 1U));
        priorityGrid.updateDynamicObstacle(900, {640, 0, 647, 719});
        NavigationWorkQueue priorityWork;
        constexpr int actors = 24, selected = 22, leased = 23;
        const Position destination{1160, 96};
        const auto origin = [](int owner) { return Position{80 + (owner % 4) * 8, 80}; };
        priorityWork.beginFrame(priorityGrid, 0, 257);
        for (int owner = 0; owner < actors; ++owner)
            static_cast<void>(priorityWork.query(owner, priorityGrid, origin(owner), destination));
        const std::array<std::uint64_t, 1> acceptedOwners{leased};
        const std::array<std::uint64_t, 1> selectedOwners{selected};
        priorityWork.prioritizeOwners(acceptedOwners);
        const auto prioritizeSelected = []<typename Queue>(Queue& queue,
            std::span<const std::uint64_t> owners) {
            if constexpr (requires { queue.prioritizeInspectionOwners(owners); })
                queue.prioritizeInspectionOwners(owners);
        };
        prioritizeSelected(priorityWork, selectedOwners);
        std::array<bool, actors> settled{};
        std::vector<int> completionOrder;
        std::array<int, actors> completedAt{};
        for (int frame = 1; frame < 15000 && completionOrder.size() < actors; ++frame) {
            priorityWork.beginFrame(priorityGrid, frame, 257);
            for (int owner = 0; owner < actors; ++owner) {
                const auto current = origin(owner);
                const auto route = priorityWork.query(owner, priorityGrid, current, destination);
                if (!route.reached() || settled[owner]) continue;
                settled[owner] = true;
                completedAt[owner] = frame;
                completionOrder.push_back(owner);
                auto safe = !route.points.empty() && route.points.front() == current &&
                    route.points.back() == destination;
                for (std::size_t leg = 1; safe && leg < route.points.size(); ++leg)
                    safe = priorityGrid.lineWalkable(route.points[leg - 1], route.points[leg]);
                check(safe, "priority scheduling published an unproved route");
            }
            check(priorityWork.diagnostics().frameWork <= 257,
                  "priority tiers exceeded the shared search and join allowance");
        }
        check(completionOrder.size() == actors,
              "selected-candidate scheduling omitted another queued actor");
        check(completionOrder.size() >= 2 && completionOrder[0] == 0 &&
                  completionOrder[1] == leased,
              "inspection priority interrupted the active frontier or outranked an accepted lease");
        check(completionOrder.size() >= 3 && completionOrder[2] == selected,
              "selected expansion candidate waited behind unrelated unleased route requests");
        std::cout << "SELECTED_ROUTE_COMPLETED_FRAME=" << completedAt[selected]
                  << ",LEASE_COMPLETED_FRAME=" << completedAt[leased]
                  << ",ALL_ROUTE_COMPLETED_FRAME=" <<
                     *std::ranges::max_element(completedAt) << '\n';
        priorityGrid.updateDynamicObstacle(901, {640, 720, 647, 799});
        priorityWork.beginFrame(priorityGrid, 15001, 0);
        check(!priorityWork.query(selected, priorityGrid, origin(selected), destination).reached(),
              "inspection priority bypassed a newly sealed route's geometry check");
    }


    {
        NavigationGrid admissionGrid(160,100,8,std::vector<std::uint8_t>(160*100,1U));
        admissionGrid.updateDynamicObstacle(990,{640,0,647,719});
        NavigationWorkQueue admissionWork;
        admissionWork.beginFrame(admissionGrid,0,257);
        const Position destination{1160,96};
        check(admissionWork.query(1,admissionGrid,{80,80},destination).searchBudgetExhausted,
              "the preferred builder retains its unfinished terrain question");
        {
            NavigationSearchAdmissionScope admission{admissionWork};
            admission.restrictNewSearches();
            for(std::uint64_t actor=2;actor<=24;++actor)
                check(admissionWork.query(actor,admissionGrid,{96,80},destination).searchBudgetExhausted,
                      "another uncertain candidate remains unknown without flooding the frontier");
            const auto ready=admissionWork.query(25,admissionGrid,{880,80},destination);
            check(ready.reached() && ready.points.front()==Position{880,80} &&
                      ready.points.back()==destination &&
                      admissionGrid.lineWalkable(ready.points.front(),ready.points.back()),
                  "an immediately proved clear candidate still proceeds during pending inspection");
            check(admissionWork.diagnostics().queued==1 &&
                      admissionWork.diagnostics().admissionDeferred==23 &&
                      admissionWork.diagnostics().frameWork<=257,
                  "uncertain candidate admission shares the allowance and keeps one queued question");
            {
                NavigationSearchAdmissionScope nested{admissionWork};
                check(!admissionWork.newSearchAdmission(),
                      "a nested inspection does not enable its restricted parent");
            }
            check(!admissionWork.newSearchAdmission(),
                  "nested destruction retains the outer admission restriction");
        }
        check(admissionWork.newSearchAdmission(),
              "returning from builder inspection restores normal search admission");
        auto reached=false;
        for(int frame=1;frame<1000 && !reached;++frame) {
            admissionWork.beginFrame(admissionGrid,frame,257);
            reached=admissionWork.query(1,admissionGrid,{80,80},destination).reached();
        }
        check(reached && admissionWork.diagnostics().searchStarts==1,
              "the retained builder route completes instead of being restarted or discarded");
        const auto before=admissionWork.diagnostics().queued;
        check(admissionWork.query(26,admissionGrid,{96,80},destination).searchBudgetExhausted &&
                  admissionWork.diagnostics().queued==before+1,
              "a later independent pass can admit the next unresolved candidate");
        try {
            NavigationSearchAdmissionScope failing{admissionWork};
            failing.restrictNewSearches();
            throw 1;
        } catch(int) {}
        check(admissionWork.newSearchAdmission(),
              "exception unwinding restores admission for future macro work");
    }

    NavigationWorkQueue travellingConstructionWork;
    NavigationPathResult savedTravelPath;
    auto travelFrame = 0;
    for (; travelFrame < 2000; ++travelFrame) {
        travellingConstructionWork.beginFrame(longRoute, travelFrame, 2048);
        savedTravelPath = travellingConstructionWork.query(41, longRoute, longFrom, longTo);
        if (savedTravelPath.reached()) break;
    }
    check(savedTravelPath.reached() && savedTravelPath.points.size() > 256,
          "the travelling builder fixture first settles a complete long terrain detour");
    auto travelRouteStayedProved = savedTravelPath.reached();
    const auto travelCompletions = travellingConstructionWork.diagnostics().completed;
    for (std::size_t i = 4; travelRouteStayedProved && i < savedTravelPath.points.size(); i += 4) {
        const auto current = savedTravelPath.points[i];
        travellingConstructionWork.beginFrame(longRoute, ++travelFrame, 2048);
        const auto joined = travellingConstructionWork.query(41, longRoute, current, longTo);
        travelRouteStayedProved = joined.reached() && !joined.points.empty() &&
            joined.points.front() == current && joined.points.back() == longTo;
        if (travelRouteStayedProved && i + 7 < savedTravelPath.points.size()) {
            const auto next = nextGroundRouteWaypoint(
                &longRoute, current, longTo, {}, 7, &travellingConstructionWork, 41);
            const auto nextIndex = std::ranges::find(savedTravelPath.points, next);
            auto prefixClear = nextIndex != savedTravelPath.points.end();
            if (prefixClear) {
                for (auto segment = savedTravelPath.points.begin() + i + 1;
                     segment <= nextIndex; ++segment)
                    prefixClear = prefixClear && longRoute.lineWalkable(*(segment-1), *segment);
            }
            check(next.valid() && nextIndex != savedTravelPath.points.end() &&
                      nextIndex > savedTravelPath.points.begin() + i &&
                      prefixClear,
                  "every forward checkpoint retains a collision-clear prefix to a future waypoint");
        }
        check(travellingConstructionWork.diagnostics().frameWork <= 2048,
              "forward builder progress retains the aggregate frame allowance");
        if (!travelRouteStayedProved)
            std::cout << "MOVING_ROUTE_FIRST_PENDING_INDEX=" << i << '\n';
    }
    check(travelRouteStayedProved && travellingConstructionWork.diagnostics().invalidated == 0 &&
              travellingConstructionWork.diagnostics().completed == travelCompletions,
          "forward progress beyond the saved route prefix preserves its proved suffix without restarting");
    auto reverseRouteStayedProved = travelRouteStayedProved;
    if (reverseRouteStayedProved) {
        auto i = ((savedTravelPath.points.size() - 1) / 4) * 4;
        while (i >= 4) {
            i -= 4;
            const auto current = savedTravelPath.points[i];
            travellingConstructionWork.beginFrame(longRoute, ++travelFrame, 2048);
            const auto joined = travellingConstructionWork.query(41, longRoute, current, longTo);
            if (!joined.reached() || joined.points.front() != current ||
                joined.points.back() != longTo) {
                reverseRouteStayedProved = false;
                break;
            }
            check(travellingConstructionWork.diagnostics().frameWork <= 2048,
                  "ordinary backward actor motion retains the same frame allowance");
        }
    }
    check(reverseRouteStayedProved && travellingConstructionWork.diagnostics().invalidated == 0 &&
              travellingConstructionWork.diagnostics().completed == travelCompletions,
          "gradual backward movement can rejoin the saved route without discarding its proof");
    auto newlySealedTravel = longRoute;
    newlySealedTravel.updateDynamicObstacle(351, {2400, 2392, 2407, 2399});
    travellingConstructionWork.beginFrame(newlySealedTravel, ++travelFrame, 2048);
    check(!travellingConstructionWork.query(41, newlySealedTravel, longFrom, longTo).reached() &&
              travellingConstructionWork.diagnostics().invalidated > 0,
          "route progress does not retain a proved suffix after a real obstacle seals the detour");

    // Queue telemetry must distinguish a retained frontier from geometry
    // restarts, completed-proof invalidation and expired idle requests.
    {
        NavigationGrid diagnosticGrid(64, 32, 8,
            std::vector<std::uint8_t>(64 * 32, 1U));
        diagnosticGrid.updateDynamicObstacle(701, {200, 64, 231, 96});
        NavigationWorkQueue diagnosticWork;
        const Position origin{80, 80}, destination{440, 80};
        diagnosticWork.beginFrame(diagnosticGrid, 0, 1);
        const auto first = diagnosticWork.query(700, diagnosticGrid, origin, destination);
        const auto initial = diagnosticWork.diagnostics();
        check(first.searchBudgetExhausted && initial.searchStarts == 1 &&
                  initial.activeOwner == 700 && initial.activeJob != 0 &&
                  initial.activeFrom == origin && initial.activeTo == destination &&
                  initial.activeAgeFrames == 0 && initial.frameWork <= 1,
              "pending queue diagnostics omitted the active query or exceeded its allowance");
        diagnosticWork.beginFrame(diagnosticGrid, 1, 1);
        static_cast<void>(diagnosticWork.query(700, diagnosticGrid, origin, destination));
        check(diagnosticWork.diagnostics().searchStarts == 1 &&
                  diagnosticWork.diagnostics().geometryRestarts == 0 &&
                  diagnosticWork.diagnostics().activeAgeFrames == 1,
              "retained search progress was misreported as a restart");
        diagnosticGrid.updateDynamicObstacle(702, {0, 200, 31, 255});
        diagnosticWork.beginFrame(diagnosticGrid, 2, 1);
        static_cast<void>(diagnosticWork.query(700, diagnosticGrid, origin, destination));
        const auto restarted = diagnosticWork.diagnostics();
        check(restarted.searchStarts == 2 && restarted.geometryRestarts == 1 &&
                  restarted.activeJob == initial.activeJob && restarted.activeAgeFrames == 2,
              "geometry restart lost the pending job identity or reset its age");
        auto completed = false;
        Frame diagnosticFrame = 2;
        for (; diagnosticFrame < 200 && !completed;) {
            diagnosticWork.beginFrame(diagnosticGrid, ++diagnosticFrame, 2048);
            completed = diagnosticWork.query(700, diagnosticGrid, origin, destination).reached();
        }
        check(completed && diagnosticWork.diagnostics().activeJob == 0,
              "a completed route remained labeled as an active frontier");
        diagnosticGrid.updateDynamicObstacle(703, {0, 160, 31, 191});
        diagnosticWork.beginFrame(diagnosticGrid, ++diagnosticFrame, 0);
        static_cast<void>(diagnosticWork.query(700, diagnosticGrid, origin, destination));
        check(diagnosticWork.diagnostics().geometryInvalidations == 1 &&
                  diagnosticWork.diagnostics().invalidated == 1,
              "completed-proof geometry invalidation was conflated with active-search restart");
        diagnosticWork.beginFrame(diagnosticGrid, diagnosticFrame + 241, 0);
        check(diagnosticWork.diagnostics().expiredJobs == 1 &&
                  diagnosticWork.diagnostics().queued == 0,
              "expired idle query diagnostics retained an orphaned request");
        diagnosticWork.reset();
        check(diagnosticWork.diagnostics().searchStarts == 0 &&
                  diagnosticWork.diagnostics().expiredJobs == 0,
              "queue reset retained telemetry from an earlier match");
    }

    NavigationGrid straightTravel(600, 64, 8, std::vector<std::uint8_t>(600 * 64, 1U));
    NavigationWorkQueue straightTravelWork;
    const Position straightFrom{80, 80}, straightTo{4600, 80};
    straightTravelWork.beginFrame(straightTravel, 0, 2048);
    check(straightTravelWork.query(42, straightTravel, straightFrom, straightTo).reached(),
          "a clear long chord is initially proved without an A* search");
    auto straightProgressProved = true;
    for (int step = 1; straightProgressProved && step <= 64; ++step) {
        const Position current{80 + step * 48, 80};
        straightTravelWork.beginFrame(straightTravel, step, 2048);
        const auto joined = straightTravelWork.query(42, straightTravel, current, straightTo);
        straightProgressProved = joined.reached() && joined.points.front() == current &&
                                 joined.points.back() == straightTo;
        if (!straightProgressProved)
            std::cout << "STRAIGHT_ROUTE_FIRST_PENDING_STEP=" << step << '\n';
    }
    check(straightProgressProved && straightTravelWork.diagnostics().invalidated == 0 &&
              straightTravelWork.diagnostics().completed == 1,
          "forward progress along a coalesced straight route stays proved without replacing its origin");
    straightTravelWork.beginFrame(straightTravel, 65, 0);
    check(straightTravelWork.query(42, straightTravel, {3200, 80}, straightTo).searchBudgetExhausted &&
              straightTravelWork.diagnostics().frameWork == 0 &&
              straightTravelWork.retainedOrigin(42, straightTo) == straightFrom,
          "a current-chord proof waits for allowance without discarding its stable origin");
    straightTravelWork.beginFrame(straightTravel, 66, 2048);
    check(straightTravelWork.query(42, straightTravel, {3200, 80}, straightTo).reached(),
          "a deferred current-chord proof resumes as soon as allowance is available");
    InfluenceMap passedStraightThreat;
    passedStraightThreat.resize(4800, 512);
    const std::array passedStorm{straightFrom};
    passedStraightThreat.updateStorms(passedStorm);
    const auto passedThreatRoute = selectSafeRouteAlternative(
        passedStraightThreat, {3200,80}, straightTo, false, &straightTravel, {},
        0.4125, {-1,-1}, 0.25, &straightTravelWork, 42);
    check(passedThreatRoute.profile.reachable && passedThreatRoute.profile.peakThreat <= 0.25 &&
              !passedThreatRoute.unresolvedAlternatives &&
              straightTravelWork.retainedOrigin(42, straightTo) == straightFrom,
          "a current-chord proof excludes passed threats without changing the detour anchor");
    InfluenceMap currentStraightThreat;
    currentStraightThreat.resize(4800, 512);
    const std::array futureStorm{straightTo};
    currentStraightThreat.updateStorms(futureStorm);
    const auto dangerousCurrentChord = selectSafeRouteAlternative(
        currentStraightThreat, {3200,80}, straightTo, false, &straightTravel, {},
        0.4125, {-1,-1}, 0.25, &straightTravelWork, 42);
    check(!dangerousCurrentChord.profile.reachable || dangerousCurrentChord.profile.peakThreat > 0.25,
          "a reused current chord still assesses new threat on its remaining route");
    auto sealedStraightTravel = straightTravel;
    sealedStraightTravel.updateDynamicObstacle(352, {4000, 0, 4007, 511});
    straightTravelWork.beginFrame(sealedStraightTravel, 67, 2048);
    check(!straightTravelWork.query(42, sealedStraightTravel, {3200, 80}, straightTo).reached(),
          "a cached coalesced route cannot bypass a new obstacle on its current chord");

    std::vector<std::uint8_t> wrongSideCells(200 * 64, 1U);
    for (int y = 0; y < 64; ++y) wrongSideCells[y * 200 + 80] = 0U;
    NavigationGrid wrongSide(200, 64, 8, std::move(wrongSideCells));
    NavigationWorkQueue wrongSideWork;
    wrongSideWork.beginFrame(wrongSide, 0, 2048);
    check(wrongSideWork.query(44, wrongSide, {80,80}, {600,80}).reached(),
          "the chord control first proves a route entirely on one side of the wall");
    wrongSideWork.beginFrame(wrongSide, 1, 2048);
    check(!wrongSideWork.query(44, wrongSide, {700,80}, {600,80}).reached(),
          "a different actor position across an unchanged wall still requires a collision-clear join");

    // A Probe-sized mover can turn through this24-pixel-wide L corridor.
    // Its only directly visible next center is8pixels away at the bend.
    std::vector<std::uint8_t> cornerCells(40 * 24, 0U);
    for (int y = 2; y <= 18; ++y)
        for (int x = 10; x <= 12; ++x) cornerCells[y * 40 + x] = 1U;
    for (int y = 16; y <= 18; ++y)
        for (int x = 10; x <= 34; ++x) cornerCells[y * 40 + x] = 1U;
    auto cornerCells32 = cornerCells;
    NavigationGrid constructionCorner(40, 24, 8, std::move(cornerCells));
    const Position cornerFrom{92,132}, cornerTo{268,140};
    const MovementFootprint cornerMover{7,7,7,7};
    const auto ordinaryCorner = constructionCorner.nextWaypoint(
        cornerFrom, cornerTo, 7, 12000, cornerMover);
    NavigationWorkQueue cornerWork;
    cornerWork.beginFrame(constructionCorner, 0, 2048);
    const auto queuedCorner = nextGroundRouteWaypoint(
        &constructionCorner, cornerFrom, cornerTo, cornerMover, 7, &cornerWork, 45);
    check(ordinaryCorner.status == NavigationStatus::reached &&
              queuedCorner == ordinaryCorner.waypoint && distance(cornerFrom, queuedCorner) >= 32,
          "queued construction lookahead matches ordinary reached-route movement through a nearby corner");
    check(!constructionCorner.lineWalkable(cornerFrom, queuedCorner, cornerMover) &&
              constructionCorner.findPath(cornerFrom, queuedCorner, 12000, cornerMover).reached(),
          "a full proved corner prefix can reach a waypoint that has no direct collision-clear chord");
    NavigationWorkQueue unfinishedCornerWork;
    unfinishedCornerWork.beginFrame(constructionCorner, 0, 1);
    check(!nextGroundRouteWaypoint(&constructionCorner, cornerFrom, cornerTo, cornerMover,
                                  7, &unfinishedCornerWork, 46).valid(),
          "corner lookahead cannot turn unfinished search knowledge into an executable prefix");
    NavigationGrid constructionCorner32(40, 24, 32, std::move(cornerCells32));
    const Position cornerFrom32{368,528}, cornerTo32{1072,560};
    const MovementFootprint cornerMover32{28,28,28,28};
    const auto ordinaryCorner32 = constructionCorner32.nextWaypoint(
        cornerFrom32, cornerTo32, 7, 12000, cornerMover32);
    NavigationWorkQueue cornerWork32;
    cornerWork32.beginFrame(constructionCorner32, 0, 2048);
    check(ordinaryCorner32.status == NavigationStatus::reached &&
              nextGroundRouteWaypoint(&constructionCorner32, cornerFrom32, cornerTo32,
                                      cornerMover32, 7, &cornerWork32, 47) == ordinaryCorner32.waypoint,
          "queued and ordinary lookahead also agree on a32-pixel navigation grid");

    NavigationWorkQueue waypointProgressWork;
    NavigationPathResult waypointProgressPath;
    auto waypointProgressFrame = 0;
    for (; waypointProgressFrame < 2000; ++waypointProgressFrame) {
        waypointProgressWork.beginFrame(longRoute, waypointProgressFrame, 2048);
        waypointProgressPath = waypointProgressWork.query(43, longRoute, longFrom, longTo);
        if (waypointProgressPath.reached()) break;
    }
    if (waypointProgressPath.reached() && waypointProgressPath.points.size() > 28) {
        const auto current = waypointProgressPath.points[20];
        waypointProgressWork.beginFrame(longRoute, ++waypointProgressFrame, 2048);
        const auto forwardWaypoint = nextGroundRouteWaypoint(
            &longRoute, current, longTo, {}, 7, &waypointProgressWork, 43);
        const auto waypointIndex = std::ranges::find(waypointProgressPath.points, forwardWaypoint);
        check(forwardWaypoint.valid() && waypointIndex != waypointProgressPath.points.end() &&
                  waypointIndex > waypointProgressPath.points.begin() + 20 &&
                  longRoute.lineWalkable(current, forwardWaypoint),
              "a builder already travelling on its proved route receives a forward rather than old-prefix waypoint");
    } else {
        check(false, "the waypoint progression fixture must first settle a long route");
    }
    NavigationWorkQueue retainedDetourWork;
    auto retainedDetoursSettled = false;
    for (auto frame = 0; frame < 800; ++frame) {
        retainedDetourWork.beginFrame(longRoute,frame,2048);
        const auto alternative = selectSafeRouteAlternative(
            InfluenceMap{},longFrom,longTo,false,&longRoute,{},0.25,{200,200},0.25,
            &retainedDetourWork,17);
        check(retainedDetourWork.diagnostics().frameWork <= 2048,
              "resumable detour legs share the aggregate construction allowance");
        if (alternative.profile.reachable && !alternative.unresolvedAlternatives) {
            retainedDetoursSettled = true;
            break;
        }
    }
    check(retainedDetoursSettled && retainedDetourWork.diagnostics().invalidated == 0,
          "fixed-origin detour legs settle without evicting their same-owner destination baseline");
    NavigationWorkQueue miningDetourWork;
    auto miningDetoursSettled = false;
    auto miningSettledFrame = 0;
    for (auto frame = 0; frame < 800; ++frame) {
        miningDetourWork.beginFrame(longRoute,frame,2048);
        const Position currentMiner{80 + (frame % 16)*4,80};
        const auto alternative = selectSafeRouteAlternative(
            InfluenceMap{},currentMiner,longTo,false,&longRoute,{},0.25,{200,200},0.25,
            &miningDetourWork,18);
        if (alternative.profile.reachable && !alternative.unresolvedAlternatives) {
            miningDetoursSettled = true;
            miningSettledFrame = frame;
            break;
        }
        check(miningDetourWork.diagnostics().frameWork <= 2048,
              "moving-miner detour evaluation retains one frame allowance");
    }
    check(miningDetoursSettled && miningDetourWork.diagnostics().invalidated == 0,
          "mining motion preserves candidate midpoints and independent fixed-origin legs");
    InfluenceMap newDestinationThreat;
    newDestinationThreat.resize(4800,2400);
    const std::array destinationStorm{longTo};
    newDestinationThreat.updateStorms(destinationStorm);
    miningDetourWork.beginFrame(longRoute,miningSettledFrame + 1,2048);
    const auto newlyThreatened = selectSafeRouteAlternative(
        newDestinationThreat,longFrom,longTo,false,&longRoute,{},0.25,{200,200},0.25,
        &miningDetourWork,18);
    check(!newlyThreatened.profile.reachable || newlyThreatened.profile.peakThreat > 0.25,
          "retained geometry cannot reuse old threat safety after a destination becomes dangerous");
    {
        NavigationObstacleOverlay newDestinationBlocker(longRoute,789,
            {longTo.x - 8,longTo.y - 8,longTo.x + 8,longTo.y + 8});
        miningDetourWork.beginFrame(longRoute,miningSettledFrame + 2,2048);
        check(!miningDetourWork.query(18,longRoute,longFrom,longTo).reached(),
              "retained actor routes are invalidated when real destination geometry is blocked");
    }
    NavigationWorkQueue independentlyAdvancedWork;
    independentlyAdvancedWork.beginFrame(longRoute,0,2048);
    check(independentlyAdvancedWork.query(7,longRoute,longFrom,longTo).searchBudgetExhausted,
          "a construction route can remain pending after its first frame");
    for (auto frame = 1; frame < 120; ++frame)
        independentlyAdvancedWork.beginFrame(longRoute,frame,2048);
    check(independentlyAdvancedWork.diagnostics().completed == 1 &&
              independentlyAdvancedWork.query(7,longRoute,longFrom,longTo).reached(),
          "frame scheduling advances requested route work even without repeated macro queries");
    NavigationWorkQueue overlayChurnWork;
    auto overlayChurnReached = false;
    const auto collisionBeforeOverlays = longRoute.collisionIdentity();
    for (auto frame = 0; frame < 500; ++frame) {
        std::vector<PlacementAccessRequirement> noAccessRequirements;
        check(placementPreservesAccess(longRoute,999,{400,1600,432,1632},noAccessRequirements),
              "a temporary remote placement candidate is legal in the retained-route fixture");
        overlayChurnWork.beginFrame(longRoute,frame,257);
        const auto result = overlayChurnWork.query(23,longRoute,longFrom,longTo);
        if (result.reached()) { overlayChurnReached = true; break; }
    }
    check(overlayChurnReached && longRoute.collisionIdentity() == collisionBeforeOverlays,
          "reversible placement overlays preserve a retained frontier's exact original geometry identity");
    NavigationGrid overlayControl{32,8,8,std::vector<std::uint8_t>(32*8,1U)};
    const auto overlayOriginalIdentity = overlayControl.collisionIdentity();
    {
        NavigationObstacleOverlay temporary{overlayControl,9,{120,0,135,63}};
        check(temporary.added() && !overlayControl.walkable({128,32}) &&
                  overlayControl.collisionIdentity() != overlayOriginalIdentity,
              "an active placement overlay has a distinct blocking geometry identity");
        overlayControl.updateDynamicObstacle(10,{200,0,215,63});
    }
    check(overlayControl.walkable({128,32}) && !overlayControl.walkable({208,32}) &&
              overlayControl.collisionIdentity() != overlayOriginalIdentity,
          "an intervening real mutation survives overlay cleanup and cannot restore stale route knowledge");
    const auto occupiedIdentity = overlayControl.collisionIdentity();
    {
        NavigationObstacleOverlay occupied{overlayControl,10,{120,0,135,63}};
        check(!occupied.added(),"a temporary placement ID cannot replace another obstacle owner");
    }
    check(!overlayControl.walkable({208,32}) && overlayControl.walkable({128,32}) &&
              overlayControl.collisionIdentity() == occupiedIdentity,
          "rejecting an occupied overlay ID preserves its existing obstacle and identity");
    {
        NavigationObstacleOverlay replaced{overlayControl,11,{120,0,135,63}};
        overlayControl.updateDynamicObstacle(11,{240,0,255,63});
    }
    check(!overlayControl.walkable({248,32}) && overlayControl.walkable({128,32}),
          "overlay destruction does not erase a deliberate replacement of its obstacle ID");
    const auto beforeUnwind = overlayControl.collisionIdentity();
    try {
        NavigationObstacleOverlay unwound{overlayControl,12,{120,0,135,63}};
        throw std::runtime_error("fixture unwind");
    } catch (const std::runtime_error&) {}
    check(overlayControl.walkable({128,32}) && overlayControl.collisionIdentity() == beforeUnwind,
          "ordinary exception unwinding restores reversible placement geometry and identity");
    longRoute.updateDynamicObstacle(250, {4544, 352, 4656, 448});
    check(continuedRoute.advance(longRoute, longFrom, longTo, 257).status == NavigationStatus::unreachable &&
              continuedRoute.diagnostics().starts == 2,
          "a changed obstacle invalidates a previously completed route before it can be reused");
    auto accessStatus = NavigationStatus::invalidInput;
    auto accessExhausted = false;
    const auto unknownAccess = groundConstructionScreeningDestination(
        InfluenceMap{}, &longRoute, longFrom, longTo, {56,56,48,48}, {}, &accessStatus, &accessExhausted);
    check(!unknownAccess.valid() && accessExhausted && (accessStatus == NavigationStatus::partial ||
                                    accessStatus == NavigationStatus::budgetExhausted),
          "an uncompleted construction approach search is not proved unreachable");
    NavigationWorkQueue approachWork;
    auto resumedApproach = Position{-1,-1};
    for (auto frame = 0; frame < 2000; ++frame) {
        approachWork.beginFrame(longRoute,frame,257);
        bool approachPending = false;
        auto approachStatus = NavigationStatus::invalidInput;
        resumedApproach = groundConstructionScreeningDestination(InfluenceMap{},&longRoute,
            longFrom,longTo,{56,56,48,48},{},&approachStatus,&approachPending,&approachWork,17);
        check(approachWork.diagnostics().frameWork <= 257,
              "construction access endpoints share the frame work allowance");
        if (resumedApproach.valid()) break;
        check(approachPending && approachStatus == NavigationStatus::budgetExhausted,
              "unfinished construction approach work remains pending rather than unsafe");
    }
    check(resumedApproach.valid() && distance(longTo,resumedApproach) <= 96 &&
              longRoute.findPath(longFrom,resumedApproach,400000).reached(),
          "a blocked construction center eventually obtains a fully proved nearby approach beyond the old quota");
    std::vector<std::uint8_t> closedCells(128 * 64, 1U);
    for (int y = 0; y < 64; ++y) closedCells[y * 128 + 64] = 0U;
    NavigationGrid closedRoute{128, 64, 8, std::move(closedCells)};
    NavigationSearch disconnectedSearch;
    auto disconnectedFinished = false;
    for (auto iteration = 0; iteration < 100; ++iteration) {
        const auto& result = disconnectedSearch.advance(closedRoute, {80,80}, {880,80}, 113);
        check(disconnectedSearch.diagnostics().stepWork <= 113,
              "disconnected component exploration is also spread across bounded steps");
        if (!result.searchBudgetExhausted) {
            disconnectedFinished = result.status == NavigationStatus::unreachable && result.points.empty();
            break;
        }
    }
    check(disconnectedFinished && disconnectedSearch.diagnostics().starts == 1,
          "a continued search eventually proves disconnection instead of staying pending forever");
    NavigationGrid branchBase{32,8,8,std::vector<std::uint8_t>(32*8,1U)};
    auto branchBlocked = branchBase, branchClear = branchBase;
    branchBlocked.updateDynamicObstacle(1, {124,0,131,63});
    branchClear.updateDynamicObstacle(1, {4,0,11,63});
    check(branchBlocked.terrainIdentity() == branchClear.terrainIdentity() &&
              branchBlocked.obstacleVersion() == branchClear.obstacleVersion() &&
              branchBlocked.collisionIdentity() != branchClear.collisionIdentity(),
          "divergent copies with equal cell revision counts retain distinct collision states");
    NavigationSearch branchSearch;
    check(branchSearch.advance(branchBlocked,{80,32},{200,32},1000).status == NavigationStatus::unreachable,
          "the first copied grid has a proved full-height blockade");
    check(branchSearch.advance(branchClear,{80,32},{200,32},1000).reached() &&
              branchSearch.diagnostics().starts == 2,
          "a copied grid cannot inherit stale completed reachability from a divergent sibling");
    NavigationSearch interruptedSearch;
    check(interruptedSearch.advance(branchBase,{80,32},{200,32},3).searchBudgetExhausted,
          "a small exact-route allowance retains an unfinished frontier");
    auto movedSearch = std::move(interruptedSearch);
    check(movedSearch.advance(branchBase,{80,32},{200,32},1000).reached() &&
              movedSearch.diagnostics().starts == 1,
          "moving an owned search retains its pending frontier");
    check(interruptedSearch.advance(branchBase,{80,32},{200,32},1000).reached(),
          "a moved-from search can start another independent query safely");
    NavigationSearch pendingMutation;
    check(pendingMutation.advance(branchBase,{80,32},{200,32},3).searchBudgetExhausted &&
              pendingMutation.advance(branchBlocked,{80,32},{200,32},1000).status == NavigationStatus::unreachable &&
              pendingMutation.diagnostics().starts == 2,
          "new collision geometry discards an in-flight frontier before its old route can escape");
    check(movedSearch.advance(branchBlocked,{80,32},{200,32},1000).status == NavigationStatus::unreachable &&
              movedSearch.diagnostics().starts == 2,
          "geometry changes also discard an in-flight owned frontier");
    NavigationGrid replacement{32,8,8,std::vector<std::uint8_t>(32*8,1U)};
    check(movedSearch.advance(replacement,{80,32},{200,32},1000).reached() &&
              movedSearch.diagnostics().starts == 3,
          "replacing terrain at the same dimensions restarts exact-route knowledge");
    const auto identityBeforeNoop = replacement.collisionIdentity();
    check(!replacement.removeDynamicObstacle(9) && replacement.collisionIdentity() == identityBeforeNoop,
          "absent obstacle removal does not invalidate a stable session");
    check(movedSearch.advance(replacement,{88,32},{200,32},1000).reached() &&
              movedSearch.diagnostics().starts == 4 &&
              movedSearch.advance(replacement,{88,32},{200,32},1000,{1,1,1,1}).reached() &&
              movedSearch.diagnostics().starts == 5,
          "changed endpoints and mover footprints start independent route proofs");
    check(movedSearch.advance(replacement,{-1,32},{200,32},1000).status == NavigationStatus::invalidInput &&
              movedSearch.diagnostics().stepWork == 0,
          "invalid incremental query inputs do no search work");
    check(movedSearch.advance(replacement,{80,32},{200,32},-1).status == NavigationStatus::invalidInput &&
              movedSearch.diagnostics().stepWork == 0,
          "negative work quotas do no search work");
    movedSearch.reset();
    check(movedSearch.diagnostics().workspaceBytes == 0 && movedSearch.diagnostics().outputBytes == 0 &&
              movedSearch.diagnostics().starts == 0,
          "reset releases owned navigation memory and stale query state");
    NavigationGrid corridor{320,1,8,std::vector<std::uint8_t>(320,1U)};
    NavigationSearch oneWorkSearch;
    auto oneWorkFinished = false;
    for (auto iteration = 0; iteration < 1100; ++iteration) {
        const auto& result = oneWorkSearch.advance(corridor,{4,4},{2524,4},1);
        check(oneWorkSearch.diagnostics().stepWork <= 1,
              "even a one-work quota advances reconstruction and output without an unbounded finish step");
        if (!result.searchBudgetExhausted) {
            oneWorkFinished = result.reached() && result.points.front() == Position{4,4} &&
                result.points.back() == Position{2524,4};
            break;
        }
    }
    check(oneWorkFinished && oneWorkSearch.diagnostics().starts == 1,
          "a one-work caller eventually receives the complete long corridor route");
    NavigationGrid corner{3,3,8,{1U,0U,1U,0U,1U,1U,1U,1U,1U}};
    NavigationSearch cornerSearch;
    check(cornerSearch.advance(corner,{4,4},{12,12},100).status == NavigationStatus::unreachable,
          "continued A* does not cut a diagonal corner between blocked orthogonal cells");
    auto closedStatus = NavigationStatus::invalidInput;
    auto closedExhausted = true;
    check(routePath({80,80}, {880,80}, false, &closedRoute, {}, &closedStatus, &closedExhausted).empty() &&
              !closedExhausted &&
              closedStatus == NavigationStatus::unreachable,
          "an exhausted disconnected component still reports proved unreachability");
    const auto closestSafePrefix = closedRoute.findPath({80,80}, {700,80});
    check(closestSafePrefix.status == NavigationStatus::partial &&
              !closestSafePrefix.searchBudgetExhausted,
          "a closest safe prefix after complete component exploration is distinct from budget exhaustion");
    auto openStatus = NavigationStatus::invalidInput;
    check(routePath({80,80}, {160,80}, false, &closedRoute, {}, &openStatus).size() == 2 &&
              openStatus == NavigationStatus::reached,
          "a clear route remains an immediately proved construction route");
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
    const Position refinery{608, 512};
    check(constructionBuilderAnchor(UnitKind::assimilator, refinery, rally, site) == refinery,
          "refinery builder selection used the army rally instead of its actual geyser");
    std::vector<RefineryConstructionSite> gasSites{
        {40, {608, 512}, true, false},
        {20, {1200, 512}, true, false},
        {10, {800, 512}, false, false},
        {5, {640, 512}, true, true},
    };
    check(selectRefineryConstructionAnchor({512, 512}, gasSites) == refinery,
          "refinery construction did not prefer an available safe completed economy");
    gasSites.erase(gasSites.begin());
    check(selectRefineryConstructionAnchor({512, 512}, gasSites) == Position{1200, 512},
          "taken main gas prevented selecting the next completed base's geyser");
    gasSites[0].threatened = true;
    check(!selectRefineryConstructionAnchor({512, 512}, gasSites).valid(),
          "refinery construction admitted an unfinished or threatened economy");
    gasSites = {{20, {600, 512}, true, false}, {10, {424, 512}, true, false}};
    check(selectRefineryConstructionAnchor({512, 512}, gasSites) == Position{424, 512} &&
          selectRefineryConstructionAnchor({-1, -1}, gasSites) == Position{424, 512},
          "refinery site selection lost its stable geyser-ID tie break");
    const Position compactRally{home.x - 128, home.y};
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, compactRally, site) == compactRally,
          "a compact static intercept keeps its supported forward anchor");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, rally, site) == home,
          "Cannon builder screening uses the base selected by the distant-rally placement fallback");
    check(constructionBuilderAnchor(UnitKind::shieldBattery, home, rally, site) == home,
          "Battery builder screening uses the base selected by the distant-rally placement fallback");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, compactRally, site,
                                    true, true) == Position{home.x - 64, home.y},
          "an active compact intercept screens builders to the same protected midpoint as placement");
    check(constructionBuilderAnchor(UnitKind::shieldBattery, home, compactRally, site,
                                    false, false) == home,
          "unknown defense direction preserves the powered base anchor");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, {home.x + 64, home.y}, site) ==
              Position{home.x + 64, home.y} &&
              constructionBuilderAnchor(UnitKind::photonCannon, home, {home.x + 256, home.y}, site) ==
              Position{home.x + 256, home.y},
          "static builder screening preserves both compact-intercept boundary distances");
    check(constructionBuilderAnchor(UnitKind::photonCannon, home, {home.x + 63, home.y}, site) == home &&
              constructionBuilderAnchor(UnitKind::photonCannon, home, {home.x + 257, home.y}, site) == home,
          "static screening follows placement's base fallback outside the intercept window");
    const Position poweredNatural{2880, 3072};
    check(constructionBuilderAnchor(UnitKind::photonCannon, poweredNatural,
                                    {poweredNatural.x + 128, poweredNatural.y}, site) ==
              Position{poweredNatural.x + 128, poweredNatural.y},
          "the chosen powered natural retains its own compact intercept rather than the main's anchor");
    check(constructionBuilderAnchor(UnitKind::photonCannon, {-1, -1}, compactRally, site) == compactRally,
          "missing base observations retain the ordinary rally fallback");
    const Position threatenedRally{1724, 404}, nativeHome{1056, 272};
    InfluenceMap defenseAnchorInfluence;
    defenseAnchorInfluence.resize(2560, 1024);
    auto rallyEnemy = UnitSnapshot{};
    rallyEnemy.id = 901;
    rallyEnemy.kind = UnitKind::marine;
    rallyEnemy.position = threatenedRally;
    rallyEnemy.hitPoints = 40;
    rallyEnemy.completed = rallyEnemy.visible = true;
    rallyEnemy.maxHitPoints = 40;
    rallyEnemy.groundWeapon.maxRange = 128;
    rallyEnemy.groundWeapon.damage = 6;
    rallyEnemy.groundWeapon.cooldown = 15;
    rallyEnemy.groundWeapon.targetsGround = true;
    GameState defenseAnchorState;
    defenseAnchorState.frame = 5800;
    defenseAnchorState.mapWidthPixels = 2560;
    defenseAnchorState.mapHeightPixels = 1024;
    defenseAnchorState.enemy.units = {rallyEnemy};
    defenseAnchorInfluence.update(defenseAnchorState);
    const auto actualStaticAnchor = constructionBuilderAnchor(
        UnitKind::photonCannon, nativeHome, threatenedRally, {-1, -1});
    check(actualStaticAnchor == nativeHome &&
              defenseAnchorInfluence.at(actualStaticAnchor).groundThreat <= 0.25F &&
              defenseAnchorInfluence.at(threatenedRally).groundThreat > 0.25F,
          "a threatened distant rally cannot reject builders for a safe home Cannon footprint");
    check(constructionBuilderAnchor(UnitKind::shieldBattery, home, {-1, -1}, site) == home,
          "missing rally did not fall back to home");
    check(homeAnchoredSupplyPylon(UnitKind::pylon,
                                  "operational supply invariant", false),
          "operational supply Pylons share the home anchor used by their travel forecast");
    // An observed Probe can finish an intermediate Move outside the 96px
    // Build handoff radius. A nearby, proved final leg is a new destination,
    // even when it lies within the old 96px last-command deadband.
    NavigationGrid shortLegNavigation(64, 48, 8, std::vector<std::uint8_t>(64 * 48, 1));
    const Position pylonCenter{400, 200};
    shortLegNavigation.updateDynamicObstacle(91, {368, 168, 432, 232});
    const Position firstLeg{444, 92};
    const auto finalApproach = nearestGroundConstructionAccessPoint(
        &shortLegNavigation, firstLeg, pylonCenter, {32, 32, 32, 32}, {8, 8, 8, 8});
    const auto secondLeg = nextGroundRouteWaypoint(
        &shortLegNavigation, firstLeg, finalApproach, {8, 8, 8, 8});
    check(finalApproach.valid() && firstLeg.valid() && secondLeg.valid() &&
        firstLeg != secondLeg && distance(firstLeg, pylonCenter) > 96 &&
        distance(secondLeg, pylonCenter) <= 96 && distance(firstLeg, secondLeg) <= 96,
        "the short-leg fixture crosses the build handoff radius with two nearby proved waypoints");
    check(!activeConstructionWaypointOrder(true, false, firstLeg, secondLeg),
        "an idle Probe's completed Move cannot suppress the next construction route leg");
    check(!activeConstructionWaypointOrder(true, true, firstLeg, secondLeg),
        "a different short construction waypoint must not be mistaken for the old Move destination");
    check(activeConstructionWaypointOrder(true, true, firstLeg, firstLeg),
        "an actually active Move to the same construction waypoint is retained");
    check(!activeConstructionWaypointOrder(true, false, firstLeg, firstLeg),
        "an interrupted or completed same-target Move may be retried after the latency guard");
    check(!activeConstructionWaypointOrder(false, true, firstLeg, firstLeg),
        "a non-Move last command cannot own the construction waypoint");
    check(activeConstructionWaypointOrder(true, true, firstLeg,
                                          {firstLeg.x + 24, firstLeg.y}) &&
          !activeConstructionWaypointOrder(true, true, firstLeg,
                                           {firstLeg.x + 25, firstLeg.y}),
        "active construction orders tolerate small target jitter without suppressing a new leg");
    check(!activeConstructionWaypointOrder(true, true, {-1, -1}, {-1, -1}) &&
          !activeConstructionWaypointOrder(true, true, firstLeg, {-1, -1}),
        "invalid targets never masquerade as an active construction route");
    check(homeAnchoredSupplyPylon(UnitKind::pylon, "maintain a supply buffer", false),
          "ordinary strategy supply Pylons stay at home");
    check(homeAnchoredSupplyPylon(UnitKind::pylon,
                                  "restore power to disabled production", false),
          "unlocated power recovery falls back to a home Pylon");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "power the new PvZ natural before pressure", false),
          "explicit natural power remains anchored at the expansion");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "operational supply invariant", true),
          "site-scoped power recovery keeps its local task anchor");
    check(!homeAnchoredSupplyPylon(UnitKind::pylon,
                                   "give the forward defensive shell redundant power", false),
          "forward-defense Pylons keep their tactical anchor");
    check(!homeAnchoredSupplyPylon(UnitKind::gateway,
                                   "operational supply invariant", false),
          "the supply placement rule does not redirect other structures");
    const Position safeNatural{2112, 3056};
    const Position safeThird{1312, 3056};
    const std::array supplyBases{
        SupplyConstructionBase{10, home, true, true},
        SupplyConstructionBase{20, safeNatural, true, false},
        SupplyConstructionBase{30, safeThird, true, false}};
    check(selectSupplyConstructionAnchor(home, supplyBases) == safeNatural,
          "supply recovery moves from the evacuated main to the nearest safe completed base");
    auto peacefulBases = supplyBases;
    peacefulBases[0].threatened = false;
    check(selectSupplyConstructionAnchor(home, peacefulBases) == home,
          "ordinary supply retains its existing home placement while the main is safe");
    auto incompleteNatural = supplyBases;
    incompleteNatural[1].completed = false;
    check(selectSupplyConstructionAnchor(home, incompleteNatural) == safeThird,
          "a warping natural cannot redirect emergency supply away from a surviving completed base");
    auto allThreatened = supplyBases;
    allThreatened[1].threatened = allThreatened[2].threatened = true;
    check(selectSupplyConstructionAnchor(home, allThreatened) == home,
          "no safe alternative preserves the original recovery attempt rather than inventing a destination");
    const std::array tiedBases{
        SupplyConstructionBase{10, home, true, true},
        SupplyConstructionBase{30, {home.x + 256, home.y}, true, false},
        SupplyConstructionBase{20, {home.x - 256, home.y}, true, false},
        SupplyConstructionBase{5, {-1, -1}, true, false}};
    check(selectSupplyConstructionAnchor(home, tiedBases) == tiedBases[2].center,
          "equidistant safe supply bases use stable unit identity and ignore invalid positions");
    check(selectSupplyConstructionAnchor(home, {}) == home &&
              selectSupplyConstructionAnchor({-1, -1}, supplyBases) == Position{-1, -1},
          "missing or invalid home observations cannot manufacture an emergency anchor");
    check(relocatableSupplyPylon(UnitKind::pylon, "operational supply invariant", false) &&
              relocatableSupplyPylon(UnitKind::pylon, "maintain a supply buffer", false) &&
              !relocatableSupplyPylon(UnitKind::pylon, "restore power to disabled production", false) &&
              !relocatableSupplyPylon(UnitKind::pylon, "operational supply invariant", true) &&
              !relocatableSupplyPylon(UnitKind::pylon, "power the new PvZ natural before pressure", false),
          "only unlocated ordinary supply can relocate; production power and site-scoped tasks retain their anchors");
    check(buildTaskTravelDeadlineFrames(UnitKind::pylon, 256, 4920, false) == 8 * 24,
          "urgent local supply retains its short recovery deadline");
    const auto remotePylonDeadline =
        buildTaskTravelDeadlineFrames(UnitKind::pylon, 1280, 4920, false);
    check(remotePylonDeadline > 8 * 24 && remotePylonDeadline < 60 * 24,
          "a distant Pylon receives a route-scaled travel window");
    check(buildTaskTravelDeadlineFrames(UnitKind::photonCannon, 0, 4920, false) == 18 * 24,
          "other constructions retain a bounded minimum acknowledgment lease");
    check(buildTaskTravelDeadlineFrames(UnitKind::pylon, 5000, 4920, true) == 60 * 24,
          "a blocked or orbiting remote worker still hits the hard lease bound");
    check(buildTaskHardTravelDeadlineFrames(UnitKind::pylon, 256, false) == 45 * 24 &&
              buildTaskHardTravelDeadlineFrames(UnitKind::nexus, 1400, false) == 120 * 24,
          "local supply and remote expansion keep distinct absolute travel caps");
    BuildTravelProgress travelProgress{
        .travelDeadline = 1000 + 36 * 24,
        .hardDeadline = 1000 + 120 * 24,
        .bestDistancePixels = 1400};
    check(recordBuildTravelProgress(travelProgress, 1000 + 30 * 24, 1370) &&
              travelProgress.travelDeadline == 1000 + 54 * 24,
          "measurable movement toward a distant footprint extends its travel lease");
    const auto deadlineAfterProgress = travelProgress.travelDeadline;
    check(!recordBuildTravelProgress(travelProgress, 1000 + 40 * 24, 1390) &&
              travelProgress.travelDeadline == deadlineAfterProgress,
          "orbiting away from the best route distance cannot extend the lease");
    check(recordBuildTravelProgress(travelProgress, 1000 + 119 * 24, 1340) &&
              travelProgress.travelDeadline == travelProgress.hardDeadline &&
              !recordBuildTravelProgress(travelProgress, travelProgress.hardDeadline, 1300),
          "route progress extends only up to the absolute travel cap");

    constexpr int navWidth = 30;
    constexpr int navHeight = 12;
    const auto makeOpenNavigation = [] {
        return NavigationGrid(navWidth, navHeight, 32,
            std::vector<std::uint8_t>(navWidth * navHeight, 1));
    };
    auto placementNavigation = makeOpenNavigation();
    PlacementAccessRequirement mineralLane;
    mineralLane.alternatives.push_back({{80, 160}, {880, 160}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> accessRequirements{mineralLane};
    capturePlacementAccessBaselines(placementNavigation, accessRequirements);
    const auto blockedLane = placementPreservesAccess(
        placementNavigation, std::uint64_t{1} << 62U, {480, 0, 511, 383},
        accessRequirements);
    check(!blockedLane && placementNavigation.findPath({80, 160}, {880, 160}).reached(),
          "a candidate that seals the only mineral corridor is rejected and its overlay is removed");
    const auto bypassLane = placementPreservesAccess(
        placementNavigation, std::uint64_t{1} << 62U, {480, 0, 511, 112},
        accessRequirements);
    check(bypassLane,
          "a candidate may touch the old route when the unit-footprint path can safely reroute");

    // A replacement terrain grid can have the same journal revision as the
    // captured map. The refreshed path must be valid on that actual terrain.
    std::vector<std::uint8_t> replacementCells(navWidth * navHeight, 1);
    for (int y = 0; y < navHeight; ++y) replacementCells[y * navWidth + 15] = 0;
    NavigationGrid replacementTerrain(navWidth, navHeight, 32, std::move(replacementCells));
    check(!placementPreservesAccess(replacementTerrain, std::uint64_t{1} << 62U,
                                   {32, 320, 63, 351}, accessRequirements, 0),
          "replacement terrain cannot reuse a failed baseline refresh with an equal journal revision");

    auto proofNavigation = makeOpenNavigation();
    auto proofRequirements = std::vector<PlacementAccessRequirement>{mineralLane};
    capturePlacementAccessBaselines(proofNavigation, proofRequirements);
    constexpr NavigationObstacleBounds distantCandidate{32, 320, 63, 351};
    constexpr auto proofCandidateId = std::uint64_t{1} << 62U;
    check(placementPreservesAccess(proofNavigation, proofCandidateId, distantCandidate,
                                  proofRequirements, 0),
          "the first validation establishes an unaffected access proof without rerouting");
    auto identicalGeometry = proofNavigation;
    auto identicalProof = proofRequirements;
    check(placementPreservesAccess(identicalGeometry, proofCandidateId, distantCandidate,
                                  identicalProof, 0),
          "an unchanged map copy can reuse its exact access proof");
    proofNavigation.updateDynamicObstacle(50, {480, 0, 511, 383});
    check(!placementPreservesAccess(proofNavigation, proofCandidateId, distantCandidate,
                                   proofRequirements, 0),
          "a real corridor seal invalidates an already cached access proof");
    static_cast<void>(proofNavigation.removeDynamicObstacle(50));
    check(placementPreservesAccess(proofNavigation, proofCandidateId, distantCandidate,
                                  proofRequirements, 0),
          "removing a real seal allows the restored baseline to be validated again");

    auto firstBranch = makeOpenNavigation();
    auto secondBranch = firstBranch;
    // The journal advances per changed cell, so both branches change12 cells.
    firstBranch.updateDynamicObstacle(51, {0, 320, 383, 351});
    secondBranch.updateDynamicObstacle(51, {480, 0, 511, 383});
    auto branchRequirements = std::vector<PlacementAccessRequirement>{mineralLane};
    capturePlacementAccessBaselines(firstBranch, branchRequirements);
    check(firstBranch.obstacleVersion() == secondBranch.obstacleVersion() &&
              firstBranch.collisionIdentity() != secondBranch.collisionIdentity() &&
              placementPreservesAccess(firstBranch, proofCandidateId, distantCandidate,
                                       branchRequirements, 0) &&
              !placementPreservesAccess(secondBranch, proofCandidateId, distantCandidate,
                                       branchRequirements, 0),
          "equal journal revisions on diverged map copies cannot reuse a different collision proof");

    auto changedEndpoint = identicalProof;
    changedEndpoint.front().alternatives.front().to = {-1, -1};
    check(!placementPreservesAccess(identicalGeometry, proofCandidateId, distantCandidate,
                                   changedEndpoint, 0),
          "changing an endpoint cannot inherit the original baseline proof");
    auto changedPoints = identicalProof;
    changedPoints.front().alternatives.front().baseline.points[1] = {-1, -1};
    check(!placementPreservesAccess(identicalGeometry, proofCandidateId, distantCandidate,
                                   changedPoints, 0),
          "editing the same baseline vector in place invalidates its immutable proof copy");
    auto changedStatus = identicalProof;
    changedStatus.front().alternatives.front().baseline.status = NavigationStatus::partial;
    check(!placementPreservesAccess(identicalGeometry, proofCandidateId, distantCandidate,
                                   changedStatus, 0),
          "a partial route cannot inherit a previously reached route's proof");

    auto narrowProofNavigation = makeOpenNavigation();
    narrowProofNavigation.updateDynamicObstacle(52, {0, 0, 959, 127});
    narrowProofNavigation.updateDynamicObstacle(53, {0, 192, 959, 383});
    auto narrowRequirements = std::vector<PlacementAccessRequirement>{mineralLane};
    capturePlacementAccessBaselines(narrowProofNavigation, narrowRequirements);
    check(narrowRequirements.front().reachableBefore &&
              placementPreservesAccess(narrowProofNavigation, proofCandidateId, distantCandidate,
                                       narrowRequirements, 0),
          "the small mover establishes a valid proof through the narrow corridor");
    narrowRequirements.front().alternatives.front().footprint = {40, 40, 40, 40};
    check(!placementPreservesAccess(narrowProofNavigation, proofCandidateId, distantCandidate,
                                   narrowRequirements, 0),
          "a wider movement footprint must be validated despite identical map and baseline points");

    // Exercise repeated candidates without changes to the proved access lanes.
    // Timing is recorded for comparison; machine-dependent time is not a gate.
    NavigationGrid repeatedCandidates(600, 300, 8, std::vector<std::uint8_t>(600 * 300, 1));
    std::vector<PlacementAccessRequirement> repeatedRequirements;
    for (int lane = 0; lane < 13; ++lane) {
        PlacementAccessRequirement laneRequirement;
        laneRequirement.alternatives.push_back({{80, 80 + lane * 16},
                                                {4600, 80 + lane * 16}, {}, {}, 0});
        repeatedRequirements.push_back(std::move(laneRequirement));
    }
    capturePlacementAccessBaselines(repeatedCandidates, repeatedRequirements);
    const auto placementBenchmarkStart = std::chrono::steady_clock::now();
    for (int candidate = 0; candidate < 256; ++candidate) {
        check(placementPreservesAccess(repeatedCandidates, std::uint64_t{1} << 62U,
                                      {2000, 2000, 2031, 2031}, repeatedRequirements, 0),
              "unaffected access remains proved across all repeated tile candidates");
    }
    std::cout << "PLACEMENT_BASELINE_REFRESH_256_US="
              << std::chrono::duration_cast<std::chrono::microseconds>(
                     std::chrono::steady_clock::now() - placementBenchmarkStart).count()
              << '\n';

    // Repeated macro passes must preserve the same access coverage without
    // rebuilding every baseline, even when no fresh search budget is given.
    NavigationGrid repeatedDetour(64, 32, 8, std::vector<std::uint8_t>(64 * 32, 1));
    std::vector<PlacementAccessRequirement> detourRequirements{
        {{{{80, 80}, {440, 80}, {}, {}, 0}}, false}};
    capturePlacementAccessBaselines(repeatedDetour, detourRequirements);
    check(placementPreservesAccess(repeatedDetour, 997, {200, 64, 231, 96}, detourRequirements),
          "the first obstructing placement has a fully searched alternative route");
    for (int retry = 0; retry < 12; ++retry)
        check(placementPreservesAccess(repeatedDetour, 997, {200, 64, 231, 96},
                                      detourRequirements, 0),
              "a repeated candidate retains its fully validated access detour without another search");
    const auto repeatedCaptureSearches = NavigationGrid::diagnosticsForCurrentThread().searches;
    for (int retry = 0; retry < 12; ++retry)
        capturePlacementAccessBaselines(repeatedCandidates, repeatedRequirements);
    check(NavigationGrid::diagnosticsForCurrentThread().searches == repeatedCaptureSearches,
          "repeated baseline capture does not rerun already validated searches");
    capturePlacementAccessBaselines(repeatedCandidates, repeatedRequirements, 0);
    check(std::ranges::all_of(repeatedRequirements, [](const auto& requirement) {
              return requirement.reachableBefore;
          }), "repeated baseline capture retains all thirteen validated access groups");

    repeatedCandidates.updateDynamicObstacle(991, {2000, 0, 2031, 4895});
    capturePlacementAccessBaselines(repeatedCandidates, repeatedRequirements);
    check(std::ranges::none_of(repeatedRequirements, [](const auto& requirement) {
              return requirement.reachableBefore;
          }), "a new full-height seal invalidates every cached access route");
    repeatedCandidates.removeDynamicObstacle(991);
    capturePlacementAccessBaselines(repeatedCandidates, repeatedRequirements);
    check(std::ranges::all_of(repeatedRequirements, [](const auto& requirement) {
              return requirement.reachableBefore;
          }), "previously unreachable access is searched again after a seal is removed");
    auto editedCapture = repeatedRequirements;
    editedCapture.front().alternatives.front().to = {-1, -1};
    capturePlacementAccessBaselines(repeatedCandidates, editedCapture, 0);
    check(!editedCapture.front().reachableBefore,
          "an edited endpoint cannot inherit a captured route during recapture");

    constexpr int routeWidth = 36;
    constexpr int routeHeight = 20;
    std::vector<std::uint8_t> routeWalkable(routeWidth * routeHeight, 1);
    for (auto y = 0; y < 16; ++y)
        routeWalkable[static_cast<std::size_t>(y * routeWidth + 17)] = 0;
    NavigationGrid constructionRoute(
        routeWidth, routeHeight, 32, std::move(routeWalkable));
    const Position routeStart{144, 336};
    const Position routeGoal{976, 336};
    const auto firstBuildWaypoint = nextGroundRouteWaypoint(
        &constructionRoute, routeStart, routeGoal, {}, 4);
    check(!constructionRoute.lineWalkable(routeStart, routeGoal) &&
              firstBuildWaypoint.valid() && firstBuildWaypoint != routeGoal &&
              constructionRoute.lineWalkable(routeStart, firstBuildWaypoint) &&
              distance(firstBuildWaypoint, routeGoal) < distance(routeStart, routeGoal),
          "a blocked long construction route yields a reachable intermediate command target");

    auto approachNavigation = makeOpenNavigation();
    const Position constructionCenter{480, 192};
    const MovementFootprint gatewayFootprint{64, 64, 48, 48};
    const MovementFootprint probeFootprint{8, 8, 8, 8};
    check(approachNavigation.updateDynamicObstacle(
              79, {constructionCenter.x - gatewayFootprint.left,
                   constructionCenter.y - gatewayFootprint.up,
                   constructionCenter.x + gatewayFootprint.right,
                   constructionCenter.y + gatewayFootprint.down}),
          "the pending structure reserves its exact footprint for route planning");
    const auto approachSearchesBefore = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto buildApproach = nearestGroundConstructionAccessPoint(
        &approachNavigation, {144, 192}, constructionCenter,
        gatewayFootprint, probeFootprint);
    check(NavigationGrid::diagnosticsForCurrentThread().searches == approachSearchesBefore,
          "a clear construction approach validates directly instead of searching every footprint side");
    check(buildApproach.valid() &&
              distance(buildApproach, constructionCenter) <= 96 &&
              approachNavigation.findPath({144, 192}, buildApproach, 12000,
                                          probeFootprint).reached(),
          "the final staged route ends beside the footprint within construction command range");

    auto builderScreening = makeOpenNavigation();
    const Position distantCenter{864, 192};
    const Position distantBuilder{80, 192};
    builderScreening.updateDynamicObstacle(90, {800, 144, 928, 240});
    InfluenceMap noThreat;
    const auto oldCenterRoute = selectSafeRouteAlternative(
        noThreat, distantBuilder, distantCenter, false, &builderScreening, probeFootprint);
    const auto screeningDestination = groundConstructionScreeningDestination(
        noThreat, &builderScreening, distantBuilder, distantCenter, gatewayFootprint, probeFootprint);
    const auto screenedRoute = selectSafeRouteAlternative(
        noThreat, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(!oldCenterRoute.profile.reachable && screenedRoute.profile.reachable &&
          screeningDestination != distantCenter && distance(screeningDestination, distantCenter) <= 96 &&
          screenedRoute.profile.peakThreat <= 0.25,
          "a blocked construction center cannot reject a distant builder with a proved safe adjacent approach");
    const auto clearScreenSearches = NavigationGrid::diagnosticsForCurrentThread().searches;
    const auto clearCenter = groundConstructionScreeningDestination(
        noThreat, &builderScreening, distantBuilder, {480, 192}, gatewayFootprint, probeFootprint);
    check(clearCenter == Position{480, 192} &&
          NavigationGrid::diagnosticsForCurrentThread().searches == clearScreenSearches,
          "walkable construction centers preserve screening and add no access searches");
    GameState guardedApproach;
    guardedApproach.mapWidthPixels = 1024;
    guardedApproach.mapHeightPixels = 384;
    UnitSnapshot approachCannon;
    approachCannon.id = 91;
    approachCannon.kind = UnitKind::photonCannon;
    approachCannon.position = screeningDestination;
    approachCannon.completed = approachCannon.visible = approachCannon.detected = approachCannon.powered = true;
    approachCannon.hitPoints = approachCannon.maxHitPoints = 100;
    approachCannon.groundWeapon = {40, 15, 0, 320, DamageType::normal, false, true};
    guardedApproach.enemy.units.push_back(approachCannon);
    InfluenceMap approachThreat;
    approachThreat.update(guardedApproach);
    const auto unsafeScreenedRoute = selectSafeRouteAlternative(
        approachThreat, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(!unsafeScreenedRoute.profile.reachable || unsafeScreenedRoute.profile.peakThreat > 0.25,
          "an occupied-center access point still fails builder screening when its approach is dangerous");
    InfluenceMap threatenedCenter;
    threatenedCenter.resize(1024, 384);
    const std::array centerStorm{Position{distantCenter.x + 80, distantCenter.y}};
    threatenedCenter.updateStorms(centerStorm);
    const auto safeOutsideStorm = selectSafeRouteAlternative(
        threatenedCenter, distantBuilder, screeningDestination, false, &builderScreening,
        probeFootprint, 0.4125, {-1, -1}, 0.25);
    check(safeOutsideStorm.profile.reachable && safeOutsideStorm.profile.peakThreat <= 0.25 &&
          !groundConstructionScreeningDestination(
              threatenedCenter, &builderScreening, distantBuilder, distantCenter,
              gatewayFootprint, probeFootprint).valid(),
          "a safe adjacent approach cannot bypass the original danger check at the structure center");
    const auto nearbyBuilder = screeningDestination;
    check(distance(nearbyBuilder, distantCenter) <= 96 &&
              groundConstructionSiteSafe(threatenedCenter, nearbyBuilder) &&
              !groundConstructionSiteSafe(threatenedCenter, distantCenter) &&
              !groundConstructionScreeningDestination(
                  threatenedCenter, &builderScreening, nearbyBuilder, distantCenter,
                  gatewayFootprint, probeFootprint).valid(),
          "a Probe already beside the footprint cannot bypass a dangerous construction site");
    check(groundConstructionSiteSafe(noThreat, distantCenter) &&
              !groundConstructionSiteSafe(noThreat, {-1, -1}),
          "site admission allows cleared locations and rejects missing construction coordinates");
    // Known contacts from the lost-natural trace: the Probe's staging cell is
    // safe, but the Nexus center still lies in the remembered army's influence.
    GameState lostNatural;
    lostNatural.frame = 13165;
    lostNatural.mapWidthPixels = lostNatural.mapHeightPixels = 4096;
    const std::array<std::array<int, 3>, 12> rememberedContacts{{
        {3689, 1554, 12798}, {3719, 1543, 12882}, {3607, 1545, 12994},
        {3616, 1543, 12866}, {3623, 1566, 12798}, {3624, 1545, 12951},
        {3599, 1544, 12912}, {3638, 1545, 13011},
        {3642, 1732, 12798}, {3604, 1761, 12798},
        {3655, 1791, 12798}, {3708, 1752, 12798},
    }};
    for (std::size_t i = 0; i < rememberedContacts.size(); ++i) {
        const auto tank = i >= 8;
        UnitSnapshot contact;
        contact.id = static_cast<int>(i);
        contact.kind = tank ? UnitKind::siegeTank : UnitKind::marine;
        contact.position = {rememberedContacts[i][0], rememberedContacts[i][1]};
        contact.lastSeen = rememberedContacts[i][2];
        contact.completed = true;
        contact.visible = contact.detected = false;
        contact.hitPoints = contact.maxHitPoints = tank ? 150 : 40;
        contact.groundWeapon = {tank ? 70 : 6, tank ? 75 : 15, 0,
                               tank ? 384 : 160, DamageType::normal, false, true};
        lostNatural.enemy.units.push_back(contact);
    }
    InfluenceMap rememberedSiteThreat;
    rememberedSiteThreat.update(lostNatural);
    check(groundConstructionSiteSafe(rememberedSiteThreat, {3674, 1263}) &&
              !groundConstructionSiteSafe(rememberedSiteThreat, {3680, 1328}),
          "the lost-natural army blocks a new Nexus even when its nearby Probe stands in a safe cell");
    builderScreening.updateDynamicObstacle(92, {480, 0, 511, 383});
    check(!groundConstructionScreeningDestination(
              noThreat, &builderScreening, distantBuilder, distantCenter, gatewayFootprint, probeFootprint).valid(),
          "a disconnected construction footprint cannot turn a partial route into a proved builder approach");
    check(groundConstructionScreeningDestination(
              noThreat, nullptr, distantBuilder, distantCenter, gatewayFootprint, probeFootprint) == distantCenter &&
          !groundConstructionScreeningDestination(
              noThreat, &builderScreening, {-1, -1}, distantCenter, gatewayFootprint, probeFootprint).valid(),
          "missing terrain preserves center screening while invalid builder observations stay invalid");

    auto plannedLayout = makeOpenNavigation();
    PlacementAccessRequirement producerExit;
    producerExit.alternatives.push_back({{80, 160}, {880, 160}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> plannedRequirements{producerExit};
    capturePlacementAccessBaselines(plannedLayout, plannedRequirements);
    check(plannedLayout.updateDynamicObstacle(17, {480, 0, 511, 191}),
          "the first planned footprint is present while the second is evaluated");
    const auto combinedBlock = placementPreservesAccess(
        plannedLayout, std::uint64_t{1} << 62U, {480, 192, 511, 383},
        plannedRequirements);
    check(!combinedBlock && plannedLayout.findPath({80, 160}, {880, 160}).reached(),
          "multiple planned footprints cannot close the last producer exit together");
    static_cast<void>(plannedLayout.removeDynamicObstacle(17));
    check(plannedLayout.findPath({80, 160}, {880, 160}).reached(),
          "a failed planned structure releases its footprint and restores the producer exit");

    auto clearanceNavigation = makeOpenNavigation();
    check(clearanceNavigation.updateDynamicObstacle(31, {480, 0, 511, 127}) &&
              clearanceNavigation.updateDynamicObstacle(32, {480, 256, 511, 383}),
          "the baseline corridor has a producer-sized opening");
    PlacementAccessRequirement probeLane;
    probeLane.alternatives.push_back({{80, 144}, {880, 144}, {}, {}, 0});
    std::vector<PlacementAccessRequirement> probeRequirements{probeLane};
    PlacementAccessRequirement reaverExit;
    reaverExit.alternatives.push_back(
        {{80, 144}, {880, 144}, {16, 16, 16, 16}, {}, 0});
    std::vector<PlacementAccessRequirement> reaverRequirements{reaverExit};
    capturePlacementAccessBaselines(clearanceNavigation, probeRequirements);
    capturePlacementAccessBaselines(clearanceNavigation, reaverRequirements);
    check(probeRequirements.front().reachableBefore && reaverRequirements.front().reachableBefore,
          "both workers and large produced units can use the initial exit");
    const auto narrowedProbeLane = placementPreservesAccess(
        clearanceNavigation, std::uint64_t{1} << 62U, {480, 160, 511, 255},
        probeRequirements);
    const auto narrowedReaverExit = placementPreservesAccess(
        clearanceNavigation, std::uint64_t{1} << 62U, {480, 160, 511, 255},
        reaverRequirements);
    check(narrowedProbeLane && !narrowedReaverExit,
          "a corridor that remains open for Probes is rejected when it cannot fit Reavers");

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
    check(plan.deferExpansion, "a five-second loss of vision incorrectly cleared the expansion site");
    state.frame += 60 * 24;
    coordinator.update(plan, state, {});
    check(!plan.deferExpansion, "stale invisible mobile threat held expansion funding forever");
    state.enemy.units[0].visible = true;
    auto nexus = fighter(200, UnitKind::nexus, site); nexus.completed = false;
    state.self.units.push_back(nexus);
    coordinator.update(plan, state, {site, true, 500});
    check(!plan.deferExpansion && !coordinator.releaseBuilder(),
          "site safety gate cancelled an already warping Nexus");
    GameState waitingState;
    waitingState.frame = 1000; waitingState.self.id = 1;
    auto waitingHome = fighter(500, UnitKind::nexus, home);
    waitingHome.role = UnitRole::resourceDepot;
    waitingState.self.units = {waitingHome};
    waitingState.bases = {{1,home,{home.x+160,home.y},3000,5000,1},
                          {2,site,{site.x+160,site.y},8000,5000,-1},
                          {3,{site.x+1000,site.y},{site.x+1160,site.y},8000,5000,-1}};
    for (auto& base : waitingState.bases) {
        base.mineralPatches = 8; base.groundDistanceFromMain = base.id * 1000;
    }
    const auto waitingPlan = [&]() {
        StrategicPlan next;
        next.name = "replace mining before collapse"; next.expansionTarget = next.rallyPoint = site;
        next.desiredBases = 2; next.estimatedMiningRunwayFrames = 5*60*24;
        next.desiredGasWorkers = 3;
        next.goals = {{GoalKind::expand,UnitKind::nexus,2,114,true,"replacement mining"}};
        return next;
    };
    ExpansionFeedback unknownFeedback{site}; unknownFeedback.routePending = true;
    BuildBlockerFeedback pendingObservation{UnitKind::nexus,{},BuildBlockerReason::routePending,1003,1000};
    check(pendingRouteObservationCurrent(pendingObservation,1003) &&
              pendingRouteObservationCurrent(pendingObservation,1119) &&
              !pendingRouteObservationCurrent(pendingObservation,1120) &&
              !pendingRouteObservationCurrent(pendingObservation,999),
          "recent unknown-route knowledge survives short retry expiry but never stale or future observations");
    pendingObservation.reason = BuildBlockerReason::unsafeRoute;
    check(!pendingRouteObservationCurrent(pendingObservation,1003),
          "unknown-route retention does not extend other blockers' deadlines");
    ExpansionCoordinator waitingCoordinator;
    auto waiting = waitingPlan();
    waitingCoordinator.update(waiting,waitingState,unknownFeedback);
    waitingState.frame += 30*24-1; waiting = waitingPlan();
    waitingCoordinator.update(waiting,waitingState,unknownFeedback);
    check(waiting.expansionTarget == site && !waiting.deferExpansion &&
              !waiting.expansionProtectionRequired && !waitingCoordinator.releaseBuilder() &&
              waitingCoordinator.routeKnowledgeWaitFrames() == 30*24-1 &&
              !waitingCoordinator.routeKnowledgeTimedOut(),
          "pending route knowledge gets a bounded wait without being declared unsafe");
    ++waitingState.frame; waiting = waitingPlan();
    waitingCoordinator.update(waiting,waitingState,unknownFeedback);
    check(waiting.expansionTarget == waitingState.bases.back().center && !waiting.deferExpansion &&
              waitingCoordinator.routeKnowledgeTimedOut() &&
              waitingCoordinator.routeKnowledgeWaitFrames() == 30*24,
          "an expansion awaiting route knowledge cannot wait forever instead of selecting an alternative");
    auto strandedState = waitingState; strandedState.frame = 1000;
    strandedState.bases.pop_back();
    ExpansionCoordinator strandedCoordinator;
    waiting = waitingPlan(); strandedCoordinator.update(waiting,strandedState,unknownFeedback);
    strandedState.frame += 30*24; waiting = waitingPlan();
    strandedCoordinator.update(waiting,strandedState,unknownFeedback);
    check(waiting.deferExpansion && waiting.name.find("[survival:") != std::string::npos &&
              waiting.desiredGasWorkers == 0 && waiting.desiredBases == 1 &&
              std::ranges::any_of(waiting.goals,[](const ProductionGoal& goal) {
                  return goal.target == UnitKind::probe && goal.blocking && goal.priority >= 118;
              }),
          "unresolved expansion with no alternative explicitly preserves remaining income before collapse");
    ExpansionCoordinator travellingCoordinator;
    auto travellingFeedback = unknownFeedback;
    travellingFeedback.pending = true; travellingFeedback.stalledFrames = 1;
    waitingState.frame = 1000; waiting = waitingPlan();
    travellingCoordinator.update(waiting,waitingState,travellingFeedback);
    waitingState.frame += 45*24; waiting = waitingPlan();
    travellingCoordinator.update(waiting,waitingState,travellingFeedback);
    check(waiting.expansionTarget == site && !waiting.deferExpansion &&
              !travellingCoordinator.releaseBuilder() &&
              travellingCoordinator.routeKnowledgeWaitFrames() == -1 &&
              !travellingCoordinator.routeKnowledgeTimedOut(),
          "a physically progressing leased builder is not abandoned by an admission-knowledge timer");
    auto stalledFeedback = travellingFeedback; stalledFeedback.stalledFrames = 8*24;
    waiting = waitingPlan(); travellingCoordinator.update(waiting,waitingState,stalledFeedback);
    check(travellingCoordinator.releaseBuilder(),
          "unknown route knowledge cannot hide an existing leased builder's physical stall");
    ExpansionCoordinator unrelatedCoordinator;
    auto unrelatedFeedback = unknownFeedback; unrelatedFeedback.site = waitingState.bases.back().center;
    waitingState.frame = 1000; waiting = waitingPlan();
    unrelatedCoordinator.update(waiting,waitingState,unrelatedFeedback);
    waitingState.frame += 45*24; waiting = waitingPlan();
    unrelatedCoordinator.update(waiting,waitingState,unrelatedFeedback);
    check(waiting.expansionTarget == site && !waiting.deferExpansion,
          "knowledge waiting at another site cannot abandon the requested expansion");
    ExpansionCoordinator recoveredCoordinator;
    waitingState.frame = 1000; waiting = waitingPlan();
    recoveredCoordinator.update(waiting,waitingState,unknownFeedback);
    waitingState.frame += 5*24; waiting = waitingPlan();
    recoveredCoordinator.update(waiting,waitingState,{});
    waitingState.frame += 10*24; waiting = waitingPlan();
    recoveredCoordinator.update(waiting,waitingState,unknownFeedback);
    waitingState.frame += 25*24; waiting = waitingPlan();
    recoveredCoordinator.update(waiting,waitingState,unknownFeedback);
    check(waiting.expansionTarget == site && !waiting.deferExpansion,
          "a resolved route interval starts a fresh knowledge wait instead of inheriting an old deadline");
    // Admission has not succeeded merely because unknown geometry and proved
    // threat alternate. Neither status should restart the total site wait.
    const auto mixedAdmission = [&](const bool endUnsafe) {
        auto mixedState = waitingState;
        mixedState.frame = 1000;
        ExpansionCoordinator mixedCoordinator;
        auto mixedPlan = waitingPlan();
        for (const auto elapsed : {0,144,288,432,576,719}) {
            mixedState.frame = 1000 + elapsed;
            mixedPlan = waitingPlan();
            ExpansionFeedback feedback{site};
            feedback.unsafeRoute = elapsed == 144 || elapsed == 432 || elapsed == 719;
            feedback.routePending = !feedback.unsafeRoute;
            mixedCoordinator.update(mixedPlan,mixedState,feedback);
            check(mixedPlan.expansionTarget == site && !mixedPlan.deferExpansion,
                  "mixed admission retains the requested site before its bounded deadline");
        }
        mixedState.frame = 1000 + 30*24;
        mixedPlan = waitingPlan();
        ExpansionFeedback feedback{site};
        feedback.unsafeRoute = endUnsafe; feedback.routePending = !endUnsafe;
        mixedCoordinator.update(mixedPlan,mixedState,feedback);
        check(mixedPlan.expansionTarget == mixedState.bases.back().center &&
                  !mixedPlan.deferExpansion && !mixedCoordinator.releaseBuilder() &&
                  mixedCoordinator.routeAdmissionTimedOut() &&
                  mixedCoordinator.routeAdmissionWaitFrames() == 30*24,
              endUnsafe ? "alternating route admission selects an alternative at its original deadline on unsafe feedback"
                        : "alternating route admission selects an alternative at its original deadline on unknown feedback");
    };
    mixedAdmission(false);
    mixedAdmission(true);
    auto warping = fighter(501,UnitKind::nexus,site); warping.completed = false;
    waitingState.self.units.push_back(warping);
    waitingState.frame += 40*24; waiting = waitingPlan();
    recoveredCoordinator.update(waiting,waitingState,unknownFeedback);
    check(!waiting.deferExpansion && !recoveredCoordinator.releaseBuilder(),
          "unknown route feedback never abandons an observed warping Nexus");
    {
        std::vector<std::uint8_t> cells(256 * 256, 1);
        for (int y = 0; y < 248; ++y) cells[y * 256 + 128] = 0;
        NavigationGrid exitGrid(256, 256, 8, std::move(cells));
        std::vector<PlacementAccessRequirement> exits(1);
        exits.front().alternatives.push_back({{32, 32}, {2016, 32}, {}});
        NavigationWorkQueue exitWork;
        PlacementAccessCaptureStatus captured = PlacementAccessCaptureStatus::deferred;
        int updates = 0; std::size_t peakWork = 0;
        for (; updates < 10000; ++updates) {
            exitWork.beginFrame(exitGrid, updates, 128);
            captured = capturePlacementAccessBaselinesBounded(exitGrid, exits, exitWork);
            peakWork = std::max(peakWork, exitWork.diagnostics().frameWork);
            if (captured == PlacementAccessCaptureStatus::complete) break;
            check(!exits.front().reachableBefore,
                  "unfinished producer-exit proof never grants reachability");
        }
        check(updates > 0 && updates < 10000 && peakWork <= 128 &&
              exits.front().reachableBefore && exits.front().alternatives.front().baseline.reached(),
              "long producer-exit proof completes over bounded retained updates");
        exitGrid.updateDynamicObstacle(99001, {1024, 1984, 1031, 2047});
        captured = PlacementAccessCaptureStatus::deferred;
        for (int retry = 0; retry < 10000; ++retry) {
            exitWork.beginFrame(exitGrid, ++updates, 128);
            captured = capturePlacementAccessBaselinesBounded(exitGrid, exits, exitWork);
            check(exitWork.diagnostics().frameWork <= 128,
                  "changed producer-exit geometry keeps the shared work cap");
            if (captured == PlacementAccessCaptureStatus::complete) break;
        }
        check(captured == PlacementAccessCaptureStatus::complete && !exits.front().reachableBefore,
              "closing the only exit invalidates a cached positive and proves failure");
        const auto searches = exitWork.diagnostics().searchStarts;
        exitWork.beginFrame(exitGrid, ++updates, 128);
        const auto repeated = capturePlacementAccessBaselinesBounded(exitGrid, exits, exitWork);
        check(repeated == PlacementAccessCaptureStatus::complete && !exits.front().reachableBefore &&
              exitWork.diagnostics().searchStarts == searches,
              "settled unreachable producer exit is reused only in unchanged geometry");
    }
    return failures ? 1 : 0;
}
