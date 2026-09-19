#pragma once
#include "protodd/PolicyLearning.hpp"
#include <fstream>
#include <string>

namespace protodd::bwapi {
// Read-only deployed policy. Training happens offline after paired validation.
class PolicyRuntime {
public:
    void start();
    PolicyAction decision();
    void end(bool won);
    bool enabled() const noexcept { return enabled_; }
private:
    PolicyLearner learner_;
    std::ofstream trace_;
    std::string context_;
    bool enabled_ = false;
    bool training_ = false;
    int nextFrame_ = 0;
    PolicyAction action_ = PolicyAction::balanced;
};
}
