#pragma once
#include "protodd/ProductionCommitments.hpp"
#include <BWAPI.h>
#include <map>
#include <ostream>

namespace protodd::bwapi {
// Observe dispatched unit production through the native contract.
// No issueCommand calls, scope replacement, or inferred completion from timeouts.
class ProductionFeedback {
public:
    void observe(int frame,std::ostream& log);
    int reserve(const BWAPI::UnitCommand& command,int action,std::ostream& log);
    void outcome(int ticket,bool accepted,std::ostream& log);
    void end(std::ostream& log) const;
private:
    struct Work {int type{},actor{},product{-1},priorProduct{-1};BWAPI::TilePosition tile;};
    ProductionCommitments ledger_;
    std::map<int,Work> work_;
    int proposal_{},frame_{-1};
};
}
