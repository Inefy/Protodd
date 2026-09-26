#pragma once
#include "protodd/ProductionDemandModel.hpp"
#include "protodd/ProductionHistory.hpp"
#include "protodd/MacroObservationStream.hpp"
#include <BWAPI.h>
#include <map>
#include <fstream>
#include "ProductionFeedback.hpp"
#include "protodd/ProductionQuota.hpp"
#include <functional>

namespace protodd::bwapi {
// Default shadow adapter; a separate local evaluation build and mode are both
// required for the early Probe/Zealot/Dragoon scope. Acceptance is recorded after
// a matching observable producer transition; API return is logged separately.
class ProductionRuntime {
public:
    void start(std::ostream& log);
    bool command(const BWAPI::UnitCommand& command, bool before, bool accepted, std::ostream& log);
    bool allows(const BWAPI::UnitCommand& command, std::string_view source) const;
    void act(Frame frame,const std::function<bool(const BWAPI::UnitCommand&)>& dispatch,std::ostream& log);
    void observe(const GameState& state, std::ostream& log);
    void infer(Frame frame, const FrameBudget& budget, std::ostream& log);
    void end(std::ostream& log);
    bool enabled() const noexcept { return observing_; }
    void disable(std::ostream& log) { fail("adapter-exception", log); }
private:
    struct Pending {
        int action{}, type{}, issued{}, queueBefore{}, orderBefore{}, buildBefore{};
        BWAPI::Position orderPositionBefore;
        BWAPI::TilePosition tile;
        bool accepted{};
        int ticket{-1};
    };
    void reconcile(Frame frame, std::ostream& log);
    void fail(std::string_view reason, std::ostream& log);
    ProductionDemandModel model_;
    ProductionHistory history_;
    MacroObservationStream observations_;
    std::vector<float> features_;
    std::map<int,Pending> pending_;
    std::ofstream inputs_;
    bool enabled_{};
    bool observing_{};
    ProductionFeedback feedback_;
    ProductionQuota quota_;
    bool control_{};
};
}
