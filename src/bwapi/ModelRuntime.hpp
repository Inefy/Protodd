#pragma once

#include "protodd/LearnedPolicy.hpp"
#include "protodd/MacroObservationStream.hpp"

#include <ostream>

namespace protodd::bwapi {

// Opt-in shadow inference only. This class has no command/execution interface.
class ModelRuntime {
public:
    void start(std::ostream& log);
    void observe(const GameState& state);
    void infer(Frame frame, const FrameBudget& budget, std::ostream& log);
    [[nodiscard]] bool enabled() const noexcept { return policy_.loaded(); }
private:
    LearnedPolicy policy_;
    MacroObservationStream observations_;
};

}  // namespace protodd::bwapi
