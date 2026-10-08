#pragma once

#include "protodd/WholeGameObservation.hpp"
#include "WholeGameCpu.hpp"
#include "WholeGameEncoder.hpp"
#include "WholeGameIntent.hpp"
#include "WholeGameSchedule.hpp"
#include "WholeGameAction.hpp"
#include <BWAPI.h>
#include <fstream>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string_view>
#include <vector>

namespace protodd::bwapi {

// v3.2 live observation and compiled policy. Control is compile-time opt-in.
class WholeGameRuntime {
public:
    void start();
    [[nodiscard]] std::vector<LegalWholeGameCommand> observe();
    void recordApiResult(const LegalWholeGameCommand& action, bool accepted,
                         int frame, std::string_view reason);
    void end();
    [[nodiscard]] bool enabled() const noexcept { return enabled_; }
    [[nodiscard]] bool modelLoaded() const noexcept { return model_ != nullptr; }
#ifdef PROTODD_ENGINE_FAULT_INJECTION
    void enableAuditProbe() noexcept { enabled_ = true; }
#endif
    // Version-1 and synthetic execution probes remain diagnostic; only the
    // six-slot architecture can replace the established tournament controller.
    [[nodiscard]] bool controlling() const noexcept {
#ifdef PROTODD_WHOLE_GAME_CONTROL
        return model_ != nullptr && model_->multiSlot();
#else
        return false;
#endif
    }
private:
    bool enabled_{};
    std::ofstream output_;
    std::ofstream inferenceOutput_;
    std::ofstream intentOutput_;
    std::ofstream actionAuditOutput_;
    std::unique_ptr<cpu::WholeGameCpu> model_;
    cpu::Terrain terrain_;
    std::vector<float> memory_;
    cpu::WholeGameSchedule schedule_;
    std::map<int, int> ids_;
    std::map<int, whole_observation::Entity> entities_;
    std::set<int> published_;
    int nextId_{}, sequence_{}, lastFrame_{-1};
    std::uint64_t nextActionAttemptId_{};
    struct PendingExecution {
        LegalWholeGameCommand action;
        int acceptedFrame{};
    };
    std::vector<PendingExecution> pendingExecutions_;
    int known(BWAPI::Unit unit) const;
    void logActionAudit(const WholeGameActionIdentity& identity, int frame,
                        int actorToken, std::string_view stage,
                        std::string_view outcome, std::string_view reason);
    void reconcileActionExecutions(int frame);
    [[nodiscard]] std::vector<LegalWholeGameCommand> dispatchDue(
        int frame, const std::vector<BWAPI::Unit>& current);
};

}  // namespace protodd::bwapi
