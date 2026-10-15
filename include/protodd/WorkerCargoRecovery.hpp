#pragma once

#include "protodd/Workers.hpp"

namespace protodd {

[[nodiscard]] inline bool shouldReturnWorkerCargo(
    const WorkerJob job, const bool carryingResources, const bool idle,
    const bool pursuingAttackTarget) noexcept {
    const auto economicAssignment = job == WorkerJob::minerals ||
                                    job == WorkerJob::gas || job == WorkerJob::transfer;
    // AttackUnit keeps chasing after the militia assignment is released.
    // Cargo does not make that order finish; hand it back to the economy.
    return carryingResources && (idle || (economicAssignment && pursuingAttackTarget));
}

} // namespace protodd
