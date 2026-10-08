"""Guard the production BWAPI command-acceptance boundaries."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DIRECT_ISSUE = re.compile(r"(?:->|\.)\s*issueCommand\s*\(")


class CommandIssuerInventoryTests(unittest.TestCase):
    def test_all_production_issue_calls_use_the_audited_boundaries(self):
        calls = []
        for path in sorted((ROOT / "src").rglob("*")):
            if path.suffix not in {".cpp", ".hpp"}:
                continue
            relative = path.relative_to(ROOT).as_posix()
            for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
                if DIRECT_ISSUE.search(line):
                    calls.append((relative, line_number, line.strip()))

        self.assertEqual(
            [(path, line) for path, _, line in calls],
            [
                ("src/bwapi/BwapiBridge.cpp",
                 "const auto accepted = command.getUnit()->issueCommand(command);"),
                ("src/bwapi/RaceBotModule.cpp",
                 "const auto accepted = actor->issueCommand(command);"),
            ],
            "Every production BWAPI issueCommand call must be added to an audited logging boundary",
        )

    def test_protodd_boundary_logs_the_bwapi_acceptance_result(self):
        source = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = source.index("bool BwapiBridge::issue(")
        end = source.index("bool BwapiBridge::requestBuildCancellation", start)
        boundary = source[start:end]

        self.assertIn("command.getUnit()->issueCommand(command)", boundary)
        self.assertIn("lastIssueError_ = Broodwar->getLastError()", boundary)
        self.assertIn("if (actionDiagnostic)", boundary)
        self.assertIn('accepted ? "accepted" : lastIssueError_.toString()', boundary)

    def test_racebot_boundary_logs_the_bwapi_acceptance_result(self):
        source = (ROOT / "src/bwapi/RaceBotModule.cpp").read_text(encoding="utf-8")
        start = source.index("bool RaceBotModule::issueCommand(")
        end = source.index("void RaceBotModule::onStart()", start)
        boundary = source[start:end]

        self.assertIn("actor->issueCommand(command)", boundary)
        self.assertIn('log_ << "COMMAND,"', boundary)
        self.assertIn('<< ",accepted=" << (accepted ? 1 : 0)', boundary)
        self.assertIn('<< ",error=" << safe(accepted ? "accepted" : error)', boundary)

    def test_learned_production_is_dispatched_through_the_bridge(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        bridge = (ROOT / "src/bwapi/BwapiBridge.hpp").read_text(encoding="utf-8")

        self.assertIn("production_.act(state_.frame", module)
        self.assertIn("return bridge_.executeProduction(command);", module)
        self.assertIn('issue(command, "production-demand")', bridge)

    def test_fresh_scout_leases_precede_worker_assignments(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("void ProtoddModule::runFrame(")
        end = module.index("void ProtoddModule::updateMacro(", start)
        frame = module[start:end]
        self.assertLess(
            frame.index('optional("scouting"'),
            frame.index('measure("workers"'),
            "a newly selected scout must be leased before same-frame worker orders are issued",
        )
        self.assertLess(
            frame.index('measure("workers"'),
            frame.index('measure("scout-command-dispatch"'),
            "scout travel must wait until same-frame worker, combat, and safety orders can preempt it",
        )
        self.assertLess(
            frame.index('measure("combat"'),
            frame.index('measure("scout-command-dispatch"'),
            "same-frame fighter and transport orders must arbitrate before scout dispatch",
        )
        self.assertLess(
            frame.index('measure("observer-safety"'),
            frame.index('measure("scout-command-dispatch"'),
            "observer safety must preempt scout travel before it is issued",
        )

        workers = module[module.index("void ProtoddModule::updateWorkers("):]
        self.assertIn("leasedScouts_.begin()", workers)
        self.assertIn("releaseScoutLeaseForPreemption(assignment.worker", workers)

        preemption_start = module.index("void ProtoddModule::releaseScoutLeaseForPreemption(")
        preemption_end = module.index("void ProtoddModule::updateScouting(", preemption_start)
        preemption = module[preemption_start:preemption_end]
        self.assertIn("scouts_.releaseLease(actor, state_.frame)", preemption)
        self.assertIn("std::erase_if(pendingScoutOrders_", preemption)
        self.assertIn("commands_.discardPending(actor, CommandOwner::scouting)", preemption)

    def test_combat_and_transport_claims_preempt_pending_scout_leases(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("void ProtoddModule::updateCombat(")
        end = module.index("void ProtoddModule::dispatchFrameCommands(", start)
        combat = module[start:end]
        self.assertIn("transportCommand.owner = CommandOwner::transport", combat)
        self.assertIn("commands_.submit", combat)
        self.assertNotIn("commands_.finalize", combat)
        self.assertNotIn("bridge_.execute(", combat)

        dispatch = module[end:module.index("std::vector<UnitSnapshot> ProtoddModule::combatUnits(", end)]
        self.assertIn("commands_.finalize(bridge_.commandBudgetRemaining())", dispatch)
        self.assertIn("commands_.authorityWinners()", dispatch)
        self.assertIn("commands_.budgetDeferredCommands()", dispatch)
        self.assertIn("releaseScoutLeaseForPreemption(command.actor", dispatch)
        self.assertIn("bridge_.executeFrameCommand(command)", dispatch)

    def test_frame_command_bus_combines_scout_combat_safety_and_maintenance(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("void ProtoddModule::runFrame(")
        end = module.index("void ProtoddModule::updateMacro(", start)
        frame = module[start:end]

        self.assertIn("bridge_.beginCommandBudget(", frame)
        self.assertLess(frame.index("bridge_.beginCommandBudget("),
                        frame.index('if (!measure("observe"'))
        self.assertIn("commands_.beginFrame(state_.frame, state_.latencyFrames)", frame)
        self.assertIn("commands_.submit(std::move(safetyCommand))", frame)
        self.assertIn("bridge_.submitScouts(pendingScoutOrders_, commands_)", frame)
        self.assertIn("bridge_.runMaintenance(plan_, commands_)", frame)
        self.assertLess(frame.index("bridge_.submitScouts(pendingScoutOrders_, commands_"),
                        frame.index('measure("workers"'))
        self.assertLess(frame.index('optional("maintenance"'), frame.index('measure("command-dispatch"'))
        self.assertLess(frame.index('measure("command-dispatch"'),
                        frame.index('measure("scout-command-dispatch"'))

        scout_start = module.index("void ProtoddModule::updateScoutMicro(")
        scout_end = module.index("void ProtoddModule::updateCombat(", scout_start)
        scout_micro = module[scout_start:scout_end]
        self.assertIn("commands_.submit(std::move(scoutingCommand))", scout_micro)
        self.assertNotIn("scoutCommands_", module)

        worker_start = module.index("void ProtoddModule::updateWorkers(")
        worker_end = module.index("void ProtoddModule::releaseScoutLeaseForPreemption(", worker_start)
        worker_phase = module[worker_start:worker_end]
        self.assertIn("bridge_.submitWorkerCommands(assignments, commands_)", worker_phase)
        self.assertNotIn("frameCommandLimit_", worker_phase)

        safety_start = frame.index('measure("observer-safety"')
        safety_end = frame.index("});", safety_start)
        safety = frame[safety_start:safety_end]
        self.assertNotIn("bridge_.execute(", safety)

    def test_bridge_enforces_command_deadline_and_lease_generation(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::execute(const Command& command")
        end = bridge.index("ExpansionFeedback BwapiBridge::expansionFeedback(", start)
        execution = bridge[start:end]
        self.assertIn('"command-deadline-expired"', execution)
        self.assertIn('"stale-command-lease"', execution)
        self.assertIn("issuedCommandLeaseGenerations_", execution)

    def test_bridge_allows_only_one_accepted_command_per_actor_per_frame(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::execute(const Command& command")
        end = bridge.index("bool BwapiBridge::executeUnchecked(", start)
        arbitration = bridge[start:end]
        self.assertIn("frameCommandClaims_.available(frame, command.actor)", arbitration)
        self.assertIn('"actor-frame-authority-already-claimed"', arbitration)
        self.assertIn("executeUnchecked(command, resourcesPaid)", arbitration)
        self.assertIn("if (accepted) frameCommandClaims_.recordAccepted(frame, command.actor)", arbitration)

        start = bridge.index("void BwapiBridge::onStart()")
        end = bridge.index("GameState BwapiBridge::observe()", start)
        self.assertIn("frameCommandClaims_.clear()", bridge[start:end])
        start = bridge.index("void BwapiBridge::forget(")
        end = bridge.index("std::vector<UnitId> BwapiBridge::reservedBuilders()", start)
        self.assertIn("frameCommandClaims_.forgetUnit(id)", bridge[start:end])

    def test_assigned_scout_uses_the_shared_command_envelope(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("std::vector<ScoutCommandFeedback> BwapiBridge::submitScouts(")
        end = bridge.index("void BwapiBridge::runMaintenance(", start)
        dispatch = bridge[start:end]
        self.assertIn("Command scoutCommand", dispatch)
        self.assertIn("scoutCommand.owner = CommandOwner::scouting", dispatch)
        self.assertIn("scoutCommand.deadlineFrame = frame", dispatch)
        self.assertIn("scoutCommand.leaseGeneration = order.leaseGeneration", dispatch)
        self.assertIn("commands.submit(std::move(scoutCommand))", dispatch)
        self.assertNotIn("execute(scoutCommand)", dispatch)

    def test_worker_orders_use_the_shared_command_envelope(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("std::size_t BwapiBridge::submitWorkerCommands(")
        end = bridge.index("std::vector<ScoutCommandFeedback> BwapiBridge::submitScouts(", start)
        dispatch = bridge[start:end]
        self.assertIn("Command command", dispatch)
        self.assertIn("command.owner = CommandOwner::worker", dispatch)
        self.assertIn("command.urgency = assignment.priority", dispatch)
        self.assertIn("command.deadlineFrame = frame", dispatch)
        self.assertIn("command.leaseGeneration = lease->second.generation", dispatch)
        self.assertIn("commands.submit(std::move(command))", dispatch)
        self.assertIn("return commands.stats().proposed - proposedBefore", dispatch)
        self.assertNotIn("execute(command)", dispatch)
        self.assertIn("CommandType::returnCargo", dispatch)
        self.assertNotRegex(dispatch, r"issue\s*\(\s*UnitCommand::")

    def test_construction_orders_use_the_shared_command_envelope(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::build(")
        end = bridge.index("bool BwapiBridge::train(", start)
        construction = bridge[start:end]
        self.assertIn("executeConstructionCommand", construction)
        self.assertNotRegex(construction, r"issue\s*\(\s*UnitCommand::")

        start = bridge.index("bool BwapiBridge::executeOwnedCommand(")
        end = bridge.index("bool BwapiBridge::executeConstructionCommand(", start)
        dispatch = bridge[start:end]
        self.assertIn("command.owner = owner", dispatch)
        self.assertIn("command.deadlineFrame = frame", dispatch)
        self.assertIn("? latest : latest + 1", dispatch)
        self.assertIn("return execute(command, resourcesPaid)", dispatch)

        start = end
        end = bridge.index("bool BwapiBridge::executeWholeGame(", start)
        construction_owner = bridge[start:end]
        self.assertIn("CommandOwner::construction", construction_owner)

        start = bridge.index("bool BwapiBridge::execute(const Command& command")
        end = bridge.index("ExpansionFeedback BwapiBridge::expansionFeedback(", start)
        execution = bridge[start:end]
        self.assertIn("case CommandType::build", execution)
        self.assertIn("type.isBuilding()", execution)
        self.assertIn("canBuildHere(tile, type, actor, true)", execution)
        self.assertIn("ResourceUse::committed, resourcesPaid", execution)
        for kind in ("trainScarab", "trainInterceptor", "feedback", "mergeArchon"):
            self.assertIn(f"case CommandType::{kind}", execution)

    def test_construction_proposals_wait_for_shared_frame_dispatch(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::executeConstructionCommand(")
        end = bridge.index("bool BwapiBridge::executeWholeGame(", start)
        proposal = bridge[start:end]
        self.assertIn("frameCommandBus_->submit(command)", proposal)
        self.assertIn("constructionCommandProposals_.push_back", proposal)
        self.assertIn("command.alreadyActive = commandActive(command)", proposal)
        self.assertIn("void BwapiBridge::beginFrameCommands", bridge)
        self.assertIn("void BwapiBridge::endFrameCommands()", bridge)
        self.assertIn("bool BwapiBridge::executeFrameCommand(", bridge)
        self.assertIn("bridge_.beginFrameCommands(commands_, state_.frame)", module)
        self.assertIn("bridge_.executeFrameCommand(command)", module)
        self.assertIn("bridge_.endFrameCommands()", module)
        self.assertIn("commands_.finalize(bridge_.commandBudgetRemaining())", module)
        self.assertNotIn("frameCommandLimit_", module)
        self.assertIn("commands_.displacedCommands()", module)
        self.assertIn("bridge_.releaseWorkerCommandLease(proposal.actor)", module)
        self.assertIn("++lease.generation", bridge)

    def test_protodd_uses_one_global_budget_for_direct_and_bus_commands(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        budget = (ROOT / "include/protodd/CommandBudget.hpp").read_text(encoding="utf-8")

        run_frame = module[
            module.index("void ProtoddModule::runFrame("):
            module.index("void ProtoddModule::updateMacro(")]
        self.assertLess(run_frame.index("bridge_.beginCommandBudget("),
                        run_frame.index('if (!measure("observe"'))
        self.assertIn("commands_.finalize(bridge_.commandBudgetRemaining())", module)
        self.assertIn("commands_.budgetDeferredCommands()", module)
        self.assertIn('"COMMAND_BUDGET_DEFERRED,"', module)
        self.assertIn("budgetDeferralAge(command.actor)", module)
        self.assertIn("defaultUrgentCommandReserve(maximumCommands)", bridge)

        issue_start = bridge.index("bool BwapiBridge::issue(")
        issue_end = bridge.index("bool BwapiBridge::requestBuildCancellation", issue_start)
        issue = bridge[issue_start:issue_end]
        self.assertLess(issue.index("commandBudget_.consume(dispatchingFrameCommand_)"),
                        issue.index("command.getUnit()->issueCommand(command)"))
        self.assertIn('"global-command-budget-deferred"', issue)

        dispatch_start = bridge.index("bool BwapiBridge::executeFrameCommand(")
        dispatch_end = bridge.index("GameState BwapiBridge::observe()", dispatch_start)
        self.assertIn("dispatchingFrameCommand_ = true", bridge[dispatch_start:dispatch_end])
        self.assertIn("if (!frameBusCommand && directUsed_ >= limit_ - reserve_)", budget)

    def test_maintenance_orders_use_the_shared_command_envelope(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("void BwapiBridge::runMaintenance(")
        end = bridge.index("void BwapiBridge::drawDebug(", start)
        maintenance = bridge[start:end]
        self.assertNotRegex(maintenance, r"issue\s*\(\s*UnitCommand::")
        self.assertIn("CommandOwner::maintenance", maintenance)
        for kind in ("trainScarab", "trainInterceptor", "feedback", "mergeArchon"):
            self.assertIn(f"CommandType::{kind}", maintenance)
        owned = bridge[
            bridge.index("bool BwapiBridge::executeOwnedCommand("):
            bridge.index("bool BwapiBridge::executeConstructionCommand(")]
        self.assertIn("command.owner = owner", owned)
        self.assertIn("command.leaseGeneration", owned)


if __name__ == "__main__":
    unittest.main()
