import json
from pathlib import Path
import unittest

from tools.feature_registry import (
    REGISTRY_PATH,
    configuration_violations,
    validate_registry,
)


class FeatureRegistryTests(unittest.TestCase):
    def setUp(self):
        self.registry = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))

    def test_registry_covers_current_build_and_runtime_controls(self):
        self.assertEqual(validate_registry(), [])
        self.assertEqual(self.registry["schema"], "protodd-feature-registry-v1")
        self.assertEqual(len(self.registry["options"]), 37)
        self.assertEqual(len(self.registry["runtime_controls"]), 10)
        for entry in [*self.registry["options"], *self.registry["runtime_controls"]]:
            for field in ("owner", "scope", "baseline", "evidence", "promotion_status", "dependencies"):
                self.assertIn(field, entry, (entry["id"], field))
                self.assertTrue(entry[field] or field == "dependencies", (entry["id"], field))

    def test_release_values_cannot_reactivate_experimental_cache_options(self):
        values = {entry["id"]: entry["tournament_value"] for entry in self.registry["options"]}
        self.assertEqual(values["PROTODD_STALLED_ARMY_ROUTING"], "ON")
        self.assertEqual(values["PROTODD_TOURNAMENT_PROFILE"], "ON")
        self.assertEqual(values["PROTODD_BUILD_BWAPI_MODULE"], "ON")
        for entry in self.registry["options"]:
            if entry["promotion_status"] == "unpromoted-opt-in":
                self.assertEqual(entry["tournament_value"], "OFF", entry["id"])
        for entry in self.registry["build_inputs"]:
            self.assertEqual(entry["tournament_value"], "")

    def test_registry_configuration_rules_reject_unsupported_combinations(self):
        violations = configuration_violations
        self.assertEqual(violations({"PROTODD_TOURNAMENT_PROFILE", "PROTODD_DEVELOPER_PROFILE"})[0],
                         "Choose either the tournament profile or the developer profile")
        self.assertIn("Unsupported combination of PvZ strategy interventions",
                      violations({"PROTODD_PVZ_GATEWAY_OPENING", "PROTODD_PVZ_REPLAY_OPENING"}))
        self.assertIn("Powered Cannon screen requires the replay PvZ opening",
                      violations({"PROTODD_PVZ_POWERED_CANNON_SCREEN"}))
        self.assertIn("Hybrid control requires a weighted local evaluation controller",
                      violations({"PROTODD_WHOLE_GAME_HYBRID", "PROTODD_WHOLE_GAME_CONTROL"}))
        self.assertEqual(violations({"PROTODD_BUILD_BWAPI_MODULE", "PROTODD_TOURNAMENT_PROFILE"}), [])


if __name__ == "__main__":
    unittest.main()
