#pragma once

#include "protodd/WholeGameObservation.hpp"
#include "WholeGameCpu.hpp"
#include "WholeGameEncoder.hpp"
#include "WholeGameIntent.hpp"
#include "WholeGameSchedule.hpp"
#include "WholeGameAction.hpp"
#include <BWAPI.h>
#include <fstream>
#include <map>
#include <memory>
#include <set>

namespace protodd::bwapi {

// v3.2 live observation and compiled policy. Control is compile-time opt-in.
class WholeGameRuntime {
public:
    void start();
    [[nodiscard]] std::vector<LegalWholeGameCommand> observe();
    void end();
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    // Version-1 and synthetic execution probes remain diagnostic; only the
    // six-slot architecture can replace the established tournament controller.
    [[nodiscard]] bool controlling() const noexcept {
        return model_ != nullptr && model_->multiSlot();
    }
private:
    bool enabled_{};
    std::ofstream output_;
    std::ofstream inferenceOutput_;
    std::ofstream intentOutput_;
    std::unique_ptr<cpu::WholeGameCpu> model_;
    cpu::Terrain terrain_;
    std::vector<float> memory_;
    cpu::WholeGameSchedule schedule_;
    std::map<int, int> ids_;
    std::map<int, whole_observation::Entity> entities_;
    std::set<int> published_;
    int nextId_{}, sequence_{}, lastFrame_{-1};
    int known(BWAPI::Unit unit) const;
    [[nodiscard]] std::vector<LegalWholeGameCommand> dispatchDue(
        int frame, const std::vector<BWAPI::Unit>& current);
};

}  // namespace protodd::bwapi
