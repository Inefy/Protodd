"""Guard the production BWAPI command-acceptance boundaries."""

from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
DIRECT_ISSUE = re.compile(r"(?:->|\.)\s*issueCommand\s*\(")


class CommandIssuerInventoryTests(unittest.TestCase):
    def test_all_production_issue_calls_use_the_audited_boundaries(self):
        calls = []
        for path in sorted(path for directory in ("src", "include")
                           for path in (ROOT / directory).rglob("*")):
            if path.suffix not in {".cpp", ".hpp"}:
                continue
            relative = path.relative_to(ROOT).as_posix()
            source = path.read_text(encoding="utf-8")
            lines = source.splitlines()
            for match in DIRECT_ISSUE.finditer(source):
                line_number = source.count("\n", 0, match.start()) + 1
                calls.append((relative, line_number, lines[line_number - 1].strip()))

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

    def test_unit_convenience_commands_cannot_bypass_logged_boundaries(self):
        # These BWAPI UnitInterface methods issue commands internally. Model
        # load, frame-load queries and opening build selection are unrelated
        # calls with overlapping names; only those exact reviewed receivers
        # are excluded from this production-source gate.
        verbs = (
            "attack move patrol holdPosition stop follow gather returnCargo repair "
            "burrow unburrow cloak decloak siege unsiege lift land load unload unloadAll "
            "rightClick haltConstruction cancelConstruction cancelAddon cancelTrain "
            "cancelMorph cancelResearch cancelUpgrade useTech build buildAddon train "
            "morph research upgrade setRallyPoint placeCOP setCommand"
        ).split()
        command_call = re.compile(r"(?:->|\.)\s*(?:" + "|".join(verbs) + r")\s*\(")
        exceptions = {
            ("include/protodd/MacroObservationStream.hpp", "budget.load"),
            ("src/bwapi/ModelRuntime.cpp", "policy_.load"),
            ("src/bwapi/ProductionRuntime.cpp", "model_.load"),
            ("src/bwapi/ProtoddModule.cpp", "tacticalTarget_.load"),
            ("src/bwapi/ProtoddModule.cpp", "allIn_.build"),
            ("src/bwapi/ProtoddModule.cpp", "frameBudget_.load"),
        }
        bypasses = []
        for path in sorted(path for directory in ("src", "include")
                           for path in (ROOT / directory).rglob("*")):
            if path.suffix not in {".cpp", ".hpp"}:
                continue
            source = path.read_text(encoding="utf-8")
            relative = path.relative_to(ROOT).as_posix()
            for match in command_call.finditer(source):
                prefix = re.search(r"[A-Za-z_]\w*\s*$", source[:match.start()])
                receiver = prefix.group().strip() if prefix else "<expression>"
                method = re.search(r"[A-Za-z_]\w*", match.group()).group()
                if (relative, receiver + "." + method) not in exceptions:
                    bypasses.append((relative, source.count("\n", 0, match.start()) + 1,
                                     receiver, method))
        self.assertEqual(bypasses, [], "BWAPI convenience orders must use the logged issue boundary")

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

        worker_start = module.index("void ProtoddModule::updateWorkers(")
        worker_end = module.index("void ProtoddModule::releaseScoutLeaseForPreemption(", worker_start)
        workers = module[worker_start:worker_end]
        self.assertIn("leasedScouts_.begin()", workers)
        self.assertNotIn("releaseScoutLeaseForPreemption(", workers,
                         "worker proposals cannot release scout state before API acceptance")

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
        self.assertIn("commands_.beginFrame(commandBudgetFrame, BWAPI::Broodwar->getLatencyFrames())", frame)
        self.assertLess(frame.index("bridge_.beginFrameCommands(commands_, commandBudgetFrame)"),
                        frame.index('if (!measure("observe"'))
        self.assertIn("~FrameCommandScope() { bridge.endFrameCommands(); }", frame)
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
        self.assertIn("if (accepted) {\n        frameCommandClaims_.recordAccepted(frame, command.actor);", arbitration)
        self.assertIn("releaseLearnedCommandLease(command, true)", arbitration)

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
        self.assertIn("bridge_.beginFrameCommands(commands_, commandBudgetFrame)", module)
        self.assertIn("bridge_.executeFrameCommand(command)", module)
        self.assertIn("bridge_.endFrameCommands()", module)
        self.assertIn("commands_.finalize(bridge_.commandBudgetRemaining())", module)
        self.assertNotIn("frameCommandLimit_", module)
        bus = (ROOT / "src/core/CommandBus.cpp").read_text(encoding="utf-8")
        self.assertIn("displacedCommands_.push_back({command, authorityWinners_.back()})", bus)
        self.assertIn("bridge_.releaseWorkerCommandLease(command.actor)", module)
        self.assertIn("bridge_.invalidateCommandLease(command.actor, CommandOwner::worker, generation)", module)
        self.assertIn("++lease.generation", bridge)

    def test_owner_release_requires_confirmed_authority(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("void ProtoddModule::dispatchFrameCommands(")
        end = module.index("void ProtoddModule::reconcileCommandEffects(", start)
        dispatch = module[start:end]
        release_start = dispatch.index("const auto releaseConfirmedAuthority")
        release_end = dispatch.index("for (const auto& command : selectedCommands) selectedActors", release_start)
        release = dispatch[release_start:release_end]
        self.assertIn("bridge_.claimConfirmedFrameAuthority(command, apiAccepted)", release)
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        claim = bridge[bridge.index("bool BwapiBridge::claimConfirmedFrameAuthority("):
                       bridge.index("bool BwapiBridge::commandActive(")]
        self.assertIn("!changesActorOrder(command.type)", claim)
        self.assertIn("!apiAccepted && (!command.alreadyActive || !commandActive(command))", claim)
        for owner_release in ("releaseScoutLeaseForPreemption(command.actor",
                              "bridge_.releaseWorkerCommandLease(command.actor)",
                              "bridge_.releaseConstructionCommandLease(command, apiAccepted)",
                              "transports_.preemptActor("):
            self.assertIn(owner_release, release)
        self.assertNotIn("commands_.displacedCommands()", release,
                         "held owner state must release even without a current queued proposal")
        self.assertIn("releaseConfirmedAuthority(command, false)", dispatch)
        execute = dispatch.index("const auto accepted = bridge_.executeFrameCommand(command)")
        confirmed = dispatch.index("releaseConfirmedAuthority(command, true)", execute)
        accepted_branch = dispatch.index("if (accepted) {", execute)
        self.assertLess(accepted_branch, confirmed)
        self.assertLess(confirmed, dispatch.index("commands_.markIssued(command)", execute))
        scout_start = module.index("void ProtoddModule::releaseScoutLeaseForPreemption(")
        scout_end = module.index("void ProtoddModule::updateScouting(", scout_start)
        self.assertIn("bridge_.invalidateCommandLease(actor, CommandOwner::scouting, generation)",
                      module[scout_start:scout_end])

    def test_native_mixed_fixture_calls_exact_production_dispatch(self):
        fixture = (ROOT / "tests/bwapi_actor_authority_scenario.cpp").read_text(encoding="utf-8")
        header = (ROOT / "src/bwapi/ProtoddModule.hpp").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("module_.dispatchFrameCommands()", fixture)
        self.assertIn("native->getOrderTargetPosition()", fixture)
        self.assertIn("liveDestinationMatches && module_.bridge_.commandActive(r.emergency)", fixture)
        self.assertIn("r.acceptedPosition)>=16*16", fixture)
        self.assertIn("distance(r.acceptedPosition,r.emergency.targetPosition)", fixture)
        self.assertIn("module_.bridge_.submitScouts", fixture)
        self.assertIn("module_.bridge_.submitWorkerCommands", fixture)
        self.assertIn("module_.bridge_.executeMacro", fixture)
        for manual_release in ("releaseScoutLeaseForPreemption", "releaseWorkerCommandLease",
                               "releaseConstructionCommandLease", "preemptActor(", "releaseLease("):
            self.assertNotIn(manual_release, fixture,
                             "native mixed proof must exercise production owner release")
        self.assertIn("#ifdef PROTODD_ACTOR_AUTHORITY_TESTS", header)
        self.assertIn("friend class ActorAuthorityFixture;", header)
        start = cmake.index("add_library(protodd_audit_actor_scenario MODULE")
        end = cmake.index("add_library(protodd_audit_load MODULE", start)
        target = cmake[start:end]
        self.assertIn("src/bwapi/ProtoddModule.cpp", target)
        self.assertIn("PROTODD_ACTOR_AUTHORITY_TESTS=1", target)
        shipping = cmake[cmake.index("add_library(protodd_bwapi"):start] if "add_library(protodd_bwapi" in cmake else cmake[:start]
        self.assertNotIn("PROTODD_ACTOR_AUTHORITY_TESTS=1", shipping)

    def test_construction_and_transport_handoff_keep_native_acknowledgment(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("std::uint64_t BwapiBridge::releaseConstructionCommandLease(")
        end = bridge.index("std::uint64_t BwapiBridge::releaseWorkerCommandLease(", start)
        handoff = bridge[start:end]
        self.assertIn("!apiAccepted && (!winner.alreadyActive || !commandActive(winner))", handoff)
        self.assertIn("pending.cancellation.handoff(frame, true)", handoff)
        self.assertNotIn("pendingBuilds_.erase", handoff)
        self.assertIn("invalidateCommandLease(winner.actor, CommandOwner::construction, generation)", handoff)
        self.assertIn("pending.cancellation.acknowledged(false, frame)", bridge)
        cancellation = (ROOT / "include/protodd/BuildCancellation.hpp").read_text(encoding="utf-8")
        self.assertIn("!handoff_ &&", cancellation)
        self.assertIn("handoff_ && observedFrame <= lastRequest_", cancellation)
        transport = (ROOT / "src/core/Transport.cpp").read_text(encoding="utf-8")
        self.assertIn("!confirmed || winner.owner == CommandOwner::transport", transport)
        self.assertIn("cargoHandoffs_.insert_or_assign", transport)
        self.assertIn('"transport-handoff-cancel-pickup"', transport)
        self.assertIn('"transport-handoff-unload"', transport)
        self.assertIn("shuttle->orderTargetId == cargo.passenger", transport)
        self.assertIn("passenger->loaded && passenger->transportId == shuttleId", transport)
        self.assertIn("state.frame >= cargo.unloadAcknowledgmentDeadline", transport)
        self.assertIn("state.frame >= mission.unloadAcknowledgmentDeadline", transport)
        self.assertIn("remainingTravel / std::max(0.5, shuttle.topSpeed)", transport)
        self.assertIn("remaining + 16.0 <= initial", transport)
        self.assertIn("state.frame < cargo.emergencyTravelDeadline", transport)
        self.assertIn("armEmergencyTravel(cargo, state, winner, shuttleId)", transport)
        self.assertIn("sameActiveIntent = winner.alreadyActive", transport)
        self.assertIn("if (!sameActiveIntent)", transport)

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



    def test_raw_commands_share_accepted_actor_frame_claims(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::issue(")
        end = bridge.index("bool BwapiBridge::requestBuildCancellation(", start)
        sink = bridge[start:end]
        claim = sink.index("frameCommandClaims_.available(frame, actorId)")
        native = sink.index("command.getUnit()->issueCommand(command)")
        record = sink.index("if (accepted) frameCommandClaims_.recordAccepted(frame, actorId)")
        self.assertLess(claim, sink.index("const auto cost = resourceCost"))
        self.assertLess(claim, sink.index("commandBudget_.consume"))
        self.assertLess(native, record)
        self.assertIn('"actor-frame-authority-already-claimed"', sink)
        self.assertIn("command.extra, false, false", sink)
        fixture = (ROOT / "tests/bwapi_actor_authority_scenario.cpp").read_text(encoding="utf-8")
        self.assertIn("executeWholeGame(a,24)", fixture)
        self.assertIn("executeProduction(b)", fixture)
        self.assertIn("raw-api-rejection-is-not-a-claim", fixture)
        self.assertIn("raw-ammunition-rejections-do-not-charge", fixture)
        self.assertNotIn("->issueCommand(", fixture)


    def test_learned_owner_preemption_requires_confirmed_higher_urgency(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::canPreemptLearnedCommandLease(")
        end = bridge.index("std::uint64_t BwapiBridge::releaseConstructionCommandLease(", start)
        handoff = bridge[start:end]
        self.assertIn("winner.effectiveUrgency() > lease->second.urgency", handoff)
        self.assertIn("changesActorOrder(winner.type)", handoff)
        self.assertIn("winner.owner != CommandOwner::learned", handoff)
        self.assertIn("(!apiAccepted && (!winner.alreadyActive || !commandActive(winner)))", handoff)
        self.assertLess(handoff.index("(!apiAccepted"), handoff.index("learnedCommandLeases_.erase"))
        self.assertIn("invalidateCommandLease(winner.actor, CommandOwner::learned, generation)", handoff)
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        self.assertIn("bridge_.releaseLearnedCommandLease(command, apiAccepted)", module)
        self.assertIn("~RestoreTypedCommand() { current = previous; }", bridge)
        fixture = (ROOT / "tests/bwapi_actor_authority_scenario.cpp").read_text(encoding="utf-8")
        self.assertIn("learned-budget-deferred-retains-owner", fixture)
        self.assertIn("learned-api-rejected-retains-owner", fixture)
        self.assertIn("learned-confirmed-retires-old-generation", fixture)
        self.assertIn("learned-lower-urgency-retains-owner", fixture)


    def test_confirmed_active_authority_blocks_fresh_raw_calls(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("bool BwapiBridge::claimConfirmedFrameAuthority(")
        end = bridge.index("bool BwapiBridge::commandActive(", start)
        claim = bridge[start:end]
        self.assertIn("!changesActorOrder(command.type)", claim)
        self.assertIn("!apiAccepted && (!command.alreadyActive || !commandActive(command))", claim)
        self.assertLess(claim.index("!apiAccepted"), claim.index("frameCommandClaims_.recordConfirmed("))
        self.assertNotIn("commandBudget_.consume", claim)
        self.assertNotIn("actionDiagnostic(", claim)
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        self.assertIn("if (!bridge_.claimConfirmedFrameAuthority(command, apiAccepted)) return;", module)
        fixture = (ROOT / "tests/bwapi_actor_authority_scenario.cpp").read_text(encoding="utf-8")
        for proof in ("learned-active-fresh-raw-rejected", "learned-active-native-winner-preserved",
                      "learned-unconfirmed-native-hint-is-false", "learned-unconfirmed-raw-retry-accepted"):
            self.assertIn(proof, fixture)
        self.assertNotIn("claimConfirmedFrameAuthority", fixture,
                         "native proof must use production dispatch to claim authority")


    def test_builder_inspection_limits_unknown_search_admission(self):
        bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")
        start = bridge.index("BWAPI::Unit BwapiBridge::findBuilder(")
        end = bridge.index("BWAPI::TilePosition BwapiBridge::buildLocation(", start)
        finder = bridge[start:end]
        self.assertIn("NavigationSearchAdmissionScope admission{constructionRouteWork_}", finder)
        self.assertEqual(finder.count("admission.restrictNewSearches()"), 2)
        navigation = (ROOT / "src/core/Navigation.cpp").read_text(encoding="utf-8")
        restricted = navigation[navigation.index("if (found == state.jobs.end() && !newSearchAdmission_)"):
                                navigation.index("if (found == state.jobs.end()) {", navigation.index("if (found == state.jobs.end() && !newSearchAdmission_)"))]
        self.assertIn("navigation.lineWalkable(from,to,footprint)", restricted)
        self.assertIn("state.frameWork", restricted)
        self.assertNotIn("state.jobs.push_back", restricted)
        self.assertNotIn("findPath(", restricted)
        fixture = (ROOT / "tests/bwapi_actor_authority_scenario.cpp").read_text(encoding="utf-8")
        self.assertIn("admission-one-uncertain-builder-route", fixture)
        self.assertNotIn("NavigationSearchAdmissionScope", fixture,
                         "native admission proof must use the real macro selector")

if __name__ == "__main__":
    unittest.main()
