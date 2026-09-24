import unittest

from training.whole_game_shadow_audit import (intent_breakdown,
                                              intent_shape_compatible,
                                              valid_intent_samples)


class ShadowIntentAuditTests(unittest.TestCase):
    def test_action_target_requirements(self):
        sample = ["24", "0.8", "attack_move", "0.6", "unit_control", "none",
                  "1", "10", "-1", "-1", "-1", "0", "0", "0", "0"]
        self.assertFalse(intent_shape_compatible(sample))
        sample[5], sample[9], sample[10] = "position", "100", "200"
        self.assertTrue(intent_shape_compatible(sample))
        sample[2], sample[5], sample[8] = "gather", "entity", "21"
        self.assertTrue(intent_shape_compatible(sample))
        sample[8] = "-1"
        self.assertFalse(intent_shape_compatible(sample))

    def test_breakdown_separates_shape_from_bwapi_legality(self):
        invalid = ["24", "0.8", "attack_move", "0.6", "unit_control", "none",
                   "1", "10", "-1", "-1", "-1", "0", "0", "0", "0", "0"]
        valid = invalid.copy()
        valid[0], valid[5], valid[9], valid[10], valid[15] = "48", "position", "100", "200", "1"
        result = intent_breakdown([invalid, valid])
        self.assertEqual(result["kinds"], {"attack_move": 2})
        self.assertEqual(result["shape_compatible_by_kind"], {"attack_move": 1})
        self.assertEqual(result["legal_by_kind"], {"attack_move": 1})
        self.assertTrue(valid_intent_samples([invalid, valid]))
        malformed = valid.copy()
        malformed[15] = "not-an-integer"
        self.assertFalse(valid_intent_samples([malformed]))


if __name__ == "__main__":
    unittest.main()
