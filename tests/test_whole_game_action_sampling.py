import collections
import unittest

from training.whole_game_action_sampling import group_seed, mixed_action_schedule


class WholeGameActionSamplingTests(unittest.TestCase):
    def test_mixed_schedule_retains_rare_commands_without_flattering_them(self):
        categories = [("PvT", "unit_control", "right_click"),
                      ("PvT", "unit_control", "attack_move"),
                      ("PvT", "production", "train")]
        counts = {("PvT", "right_click"): 900, ("PvT", "attack_move"): 90,
                  ("PvT", "train"): 10}
        schedule = mixed_action_schedule(categories, counts, 120, seed=17)
        actual = collections.Counter(schedule)
        self.assertEqual(len(schedule), 120)
        self.assertGreater(actual[categories[0]], actual[categories[1]])
        self.assertGreater(actual[categories[1]], actual[categories[2]])
        self.assertGreater(actual[categories[2]], 0)
        self.assertLess(actual[categories[0]], 90)
        self.assertEqual(schedule, mixed_action_schedule(categories, counts, 120, seed=17))

    def test_group_order_does_not_change_seed(self):
        first = {"PvT": ["a", "b"], "PvZ": ["c"]}
        second = {"PvZ": ["c"], "PvT": ["a", "b"]}
        self.assertEqual(group_seed(first), group_seed(second))

    def test_invalid_schedule_is_rejected(self):
        with self.assertRaises(ValueError):
            mixed_action_schedule([], {}, 120)
        with self.assertRaises(ValueError):
            mixed_action_schedule([("PvT", "unit_control", "move")], {}, 4)


if __name__ == "__main__":
    unittest.main()
