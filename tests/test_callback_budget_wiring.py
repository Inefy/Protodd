"""Guard callback-budget wiring from the frame loop to measured audit output."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class CallbackBudgetWiringTests(unittest.TestCase):
    def test_optional_phase_is_budgeted_before_its_operation_starts(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("void ProtoddModule::runFrame(")
        end = module.index("void ProtoddModule::updateMacro(", start)
        frame = module[start:end]
        measure_start = frame.index("const auto measure =")
        measure_end = frame.index("const auto optional =", measure_start)
        measure = frame[measure_start:measure_end]

        self.assertIn("CallbackBudget& callbackBudget", frame)
        self.assertIn("callbackBudget.allowsOptionalWork(", measure)
        self.assertLess(measure.index("callbackBudget.allowsOptionalWork("),
                        measure.index("operation();"))
        self.assertIn("timing.deferForBudget()", measure)
        self.assertIn("deferredOptionalPhases_.insert_or_assign(phase, state_.frame)", measure)
        for phase in ("whole-game-observe", "production-shadow", "model-shadow",
                      "influence", "inference", "strategy", "scouting",
                      "diagnostics", "state-log"):
            self.assertIn(f'optional("{phase}"', frame)

    def test_combat_simulation_is_optional_while_safety_control_still_runs(self):
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")
        start = module.index("if (scheduledWork.updateCombat)")
        end = module.index('if (optionalDue("maintenance"', start)
        combat = module[start:end]

        self.assertIn("callbackBudget.allowsOptionalWork(", combat)
        self.assertIn("runSimulation = callbackBudget.allowsOptionalWork(", combat)
        self.assertIn('measure("combat", [this, runSimulation] { updateCombat(runSimulation,',
                      combat)
        self.assertIn('measure("observer-safety"', combat)
        self.assertLess(combat.index("runSimulation = callbackBudget.allowsOptionalWork("),
                        combat.index("updateCombat(runSimulation,"))
        self.assertLess(combat.index("updateCombat(runSimulation,"),
                        combat.index('measure("observer-safety"'))

    def test_slow_observation_audit_records_deferral_and_full_callback_times(self):
        harness = (ROOT / "tests/bwapi_fault_scenario.cpp").read_text(encoding="utf-8")
        module = (ROOT / "src/bwapi/ProtoddModule.cpp").read_text(encoding="utf-8")

        self.assertIn('scenario_ == "slow-observation"', harness)
        self.assertIn('scenario_ == "slow-observation-model"', harness)
        self.assertIn('check("optional-phases-were-deferred-after-slow-observation"', harness)
        self.assertIn('check("combat-remained-executable-on-the-over-budget-callback"', harness)
        self.assertIn('"CALLBACK_SUMMARY,"', harness)
        self.assertIn('"CALLBACK_US,"', harness)
        self.assertIn('"AUDIT_SLOW_OBSERVATION,"', module)


if __name__ == "__main__":
    unittest.main()
