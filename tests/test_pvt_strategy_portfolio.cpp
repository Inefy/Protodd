#include "protodd/PvTStrategyPortfolio.hpp"

#include <algorithm>
#include <array>
#include <iostream>
#include <string>

namespace {
int failures = 0;
void expect(bool condition, const char* message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

std::string snapshot(std::string_view rows) {
    return "# PROTODD_PVT_PORTFOLIO 1\n# version,race,opponent,map,strategy,wins,losses\n" +
           std::string(rows);
}
}

int main() {
    using namespace protodd;
    const PvTStrategyContext context{"TerranOpponent", "terran", "Python",
                                     std::string(pvtStrategyPortfolioVersion)};
    const PvTStrategyContext otherMap{"TerranOpponent", "terran", "Destination",
                                      std::string(pvtStrategyPortfolioVersion)};
    const PvTStrategyContext wrongRace{"TerranOpponent", "protoss", "Python",
                                       std::string(pvtStrategyPortfolioVersion)};

    const auto defaultGate = pvtStrategyRuntimeGate(false, false, true, false, true, true);
    expect(!defaultGate.adaptive && !defaultGate.mayWriteHistory,
           "runtime adaptive selection is disabled when the build flag is off");
    const auto overrideGate = pvtStrategyRuntimeGate(true, true, true, false, true, true);
    expect(!overrideGate.adaptive && !overrideGate.mayWriteHistory,
           "explicit PvT strategy override takes precedence");
    expect(!pvtStrategyRuntimeGate(true, false, true, true, true, true).mayWriteHistory,
           "frozen evaluation disables portfolio history writes");
    expect(!pvtStrategyRuntimeGate(true, false, true, false, false, true).adaptive,
           "portfolio only activates against Terran");
    expect(!pvtStrategyRuntimeGate(true, false, true, false, true, false).adaptive,
           "adaptive mode requires a unique externally assigned match identity");
    const auto adaptiveGate = pvtStrategyRuntimeGate(true, false, true, false, true, true);
    expect(adaptiveGate.adaptive && adaptiveGate.mayWriteHistory,
           "explicit adaptive mode enables selection and history snapshot writes");

    PvTStrategyPortfolio portfolio;
    expect(portfolio.parse(""), "empty history is a valid cold start");
    for (std::uint64_t seed = 0; seed < 64; ++seed)
        expect(portfolio.choose(context, seed) == portfolio.choose(context, seed),
               "selection is deterministic for a seed");
    std::array<bool, 3> visited{};
    for (std::uint64_t seed = 0; seed < 16; ++seed)
        visited[static_cast<std::size_t>(portfolio.choose(context, seed))] = true;
    expect(std::ranges::all_of(visited, [](bool seen) { return seen; }),
           "seeded exploration reaches every legal untried arm");
    expect(portfolio.choose(context, 0, 0) == PvTStrategyId::standard,
           "empty legal mask falls back safely to standard");
    expect(portfolio.choose(context, 22, 1u << 2) ==
               PvTStrategyId::economicOneGatewayObserver,
           "selection honors a singleton legal-arm mask");

    const auto beforeChoice = portfolio.serialize();
    for (std::uint64_t seed = 0; seed < 32; ++seed)
        static_cast<void>(portfolio.choose(context, seed));
    expect(portfolio.serialize() == beforeChoice,
           "selection cannot change the latched strategy or update history mid-match");

    PvTStrategyPortfolio prior;
    const auto validRows = snapshot(
        "r3-siege-v3-pvt-portfolio-v1,terran,TerranOpponent,Destination,"
        "economic-1gateway-observer,4,0\n");
    expect(prior.parse(validRows), "versioned validated snapshot loads");
    expect(prior.lookup(otherMap, PvTStrategyId::economicOneGatewayObserver).wins == 4,
           "records retain opponent, map, version, race, and arm context");
    expect(prior.lookup(context, PvTStrategyId::standard).games() == 0,
           "map-specific local records do not leak across maps");
    expect(prior.lookup(wrongRace, PvTStrategyId::economicOneGatewayObserver).games() == 0,
           "opponent race is isolated");
    expect(prior.choose(context, 4, PvTStrategyPortfolio::allLegalArms, false) ==
               PvTStrategyId::economicOneGatewayObserver,
           "bounded same-opponent cross-map prior informs greedy selection");
    expect(prior.merge(snapshot(
               "r3-siege-v3-pvt-portfolio-v1,terran,TerranOpponent,Destination,"
               "economic-1gateway-observer,3,1\n")),
           "component-wise max merge accepts cumulative snapshots");
    expect(prior.lookup(otherMap, PvTStrategyId::economicOneGatewayObserver).wins == 4 &&
               prior.lookup(otherMap, PvTStrategyId::economicOneGatewayObserver).losses == 1,
           "cumulative merge is idempotent and preserves max counts");

    const auto before = prior.serialize();
    const auto badRows = std::array<std::string, 6>{
        "# PROTODD_PVT_PORTFOLIO 0\n",
        snapshot("r3-siege-v2-pvt-portfolio-v1,terran,opponent,map,standard,1,0\n"),
        snapshot("r3-siege-v3-pvt-portfolio-v1,protoss,opponent,map,standard,1,0\n"),
        snapshot("r3-siege-v3-pvt-portfolio-v1,terran,opponent,map,unknown,0,0\n"),
        snapshot("r3-siege-v3-pvt-portfolio-v1,terran,opponent,map,standard,-1,0\n"),
        "not a portfolio"};
    for (const auto& corrupt : badRows) {
        expect(!prior.parse(corrupt), "old or corrupt history is rejected");
        expect(prior.serialize() == before, "failed parse preserves usable history");
    }
    expect(!prior.parse(snapshot(std::string(pvtPortfolioMaximumLineBytes + 1, '#'))),
           "oversized comment line is bounded and rejected");
    expect(prior.serialize() == before, "oversized line rejection is transactional");
    expect(!prior.parse(std::string(pvtPortfolioMaximumBytes + 1, '#')),
           "oversized snapshot is rejected before parsing");
    expect(prior.serialize() == before, "oversized snapshot rejection preserves history");
    std::string tooManyRows = snapshot("");
    for (std::size_t i = 0; i <= pvtPortfolioMaximumRows; ++i)
        tooManyRows += "r3-siege-v3-pvt-portfolio-v1,terran,o" + std::to_string(i) +
                       ",m,standard,0,0\n";
    expect(!prior.parse(tooManyRows), "portfolio row count is bounded");
    expect(prior.serialize() == before, "row-limit rejection preserves history");

    std::cout << "PvT strategy portfolio: " << failures << " failures\n";
    return failures == 0 ? 0 : 1;
}
