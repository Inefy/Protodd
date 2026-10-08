"""Guard unit identity cleanup across BWAPI lifecycle callbacks and game resets."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class ActorLifecycleWiringTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        cls.bridge = (ROOT / "src/bwapi/BwapiBridge.cpp").read_text(encoding="utf-8")

    def test_destroy_create_and_renegade_callbacks_invalidate_actor_state(self):
        create = self.module[
            self.module.index("void ProtoddModule::onUnitCreate("):
            self.module.index("void ProtoddModule::onUnitComplete(")]
        destroy = self.module[
            self.module.index("void ProtoddModule::onUnitDestroyImpl("):
            self.module.index("void ProtoddModule::invalidateActorState(")]
        renegade = self.module[
            self.module.index("void ProtoddModule::onUnitRenegade("):
            self.module.index("void ProtoddModule::updateStrategy(")]

        self.assertIn("invalidateActorState(unit->getID())", create)
        self.assertIn("bridge_.forget(unit)", create)
        self.assertIn("invalidateActorState(unit->getID())", destroy)
        self.assertIn("bridge_.forget(unit)", destroy)
        self.assertIn("invalidateActorState(unit->getID())", renegade)
        self.assertIn("bridge_.forget(unit)", renegade)
        self.assertIn("bridge_.remember(unit)", renegade)

        invalidation = self.module[
            self.module.index("void ProtoddModule::invalidateActorState("):
            self.module.index("void ProtoddModule::onUnitMorph(")]
        for cleanup in ("commands_.forgetUnit(id)", "scouts_.forgetUnit(id)",
                        "squads_.forgetUnit(id)", "transports_.forgetUnit(id)",
                        "engagements_.forgetUnit(id)", "detectorEscorts_",
                        "leasedScouts_", "hybridProposals_", "lastActions_.erase(id)"):
            self.assertIn(cleanup, invalidation)

    def test_bridge_forget_removes_actor_references_not_only_actor_keys(self):
        start = self.bridge.index("void BwapiBridge::forget(")
        end = self.bridge.index("std::vector<UnitId> BwapiBridge::reservedBuilders(", start)
        forget = self.bridge[start:end]

        self.assertIn("selfUnitLifetimes_.erase(id)", forget)
        self.assertIn("enemyMemory_.erase(id)", forget)
        self.assertIn("entry.second.source == id || entry.second.target == id", forget)
        self.assertIn("proposal.command.actor == id || proposal.command.targetUnit == id", forget)
        self.assertIn("frameCommandClaims_.forgetUnit(id)", forget)
        self.assertIn("pending.builder == id) pending.builder = -1", forget)

    def test_same_frame_construction_proposals_reserve_their_builder(self):
        start = self.bridge.index("BWAPI::Unit BwapiBridge::findBuilder(")
        end = self.bridge.index("BWAPI::TilePosition BwapiBridge::buildLocation(", start)
        selection = self.bridge[start:end]
        self.assertIn("constructionCommandProposals_", selection)
        self.assertIn("proposal.command.actor == unit->getID()", selection)

    def test_new_game_resets_command_ownership_and_deferred_runtime_state(self):
        start = self.module.index("void ProtoddModule::onStartImpl()")
        end = self.module.index("void ProtoddModule::onEnd(", start)
        startup = self.module[start:end]

        for reset in ("bridge_.onStart()", "commands_.clear()", "scouts_.reset()",
                      "squads_.reset()", "transports_.reset()", "engagements_.reset()",
                      "urgentEvents_.clear()", "deferredOptionalPhases_.clear()",
                      "callbackTimes_.clear()", "hybridProposals_.clear()",
                      "fight_ = {}", "pendingScoutOrders_.clear()",
                      "scoutingDispatchPending_ = false"):
            self.assertIn(reset, startup)

        harness = (ROOT / "tests/bwapi_fault_scenario.cpp").read_text(encoding="utf-8")
        self.assertIn('scenario_ == "lifecycle-reset"', harness)
        self.assertIn('check("lifecycle-reset-second-session-started"', harness)


if __name__ == "__main__":
    unittest.main()
