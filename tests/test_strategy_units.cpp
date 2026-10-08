#include "StrategyDetail.hpp"

#include <iostream>
#include <string_view>

namespace {

using namespace protodd;
using namespace protodd::strategy_detail;
int failures{};

void check(const bool condition, const std::string_view message) {
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

UnitSnapshot unit(const UnitKind kind, const bool completed) {
    UnitSnapshot result;
    result.kind = kind;
    result.completed = completed;
    return result;
}

}  // namespace

int main() {
    GameState state;
    state.self.supplyUsed = 28;
    state.self.units = {
        unit(UnitKind::gateway, true),
        unit(UnitKind::gateway, false),
        unit(UnitKind::zealot, true),
    };

    check(effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::observed) == 2,
          "observed unit count includes started unfinished structures");
    check(effectiveUnitCount(state, UnitKind::gateway, UnitCountBasis::completed) == 1,
          "completed unit count excludes unfinished structures");
    check(effectiveUnitCount(state, UnitKind::zealot, UnitCountBasis::completed) == 1,
          "completed unit count includes completed mobile units");
    check(supplyAtLeast(state, DisplayedSupply{14}) &&
              !supplyAtLeast(state, DisplayedSupply{15}),
          "displayed supply thresholds account for the engine's doubled supply units");
    check(framesForSeconds(90) == 2160 && framesForMinutes(5) == 7200,
          "time-unit helpers convert seconds and minutes to 24 FPS frames");
    state.frame = framesForMinutes(5) - 1;
    check(minute(state) == 4, "minute conversion floors elapsed frames");

    if (failures == 0) {
        std::cout << "Strategy unit checks passed\n";
        return 0;
    }
    std::cerr << failures << " strategy unit check(s) failed\n";
    return 1;
}
