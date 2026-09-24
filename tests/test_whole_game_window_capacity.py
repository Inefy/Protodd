import unittest

from training.whole_game_window_capacity import summarize, window_capacity


def label(kind, mode, actor, *, domain="unit_control", position=None):
    return dict(domain=domain, actor_positive=[actor],
                actions=dict(kind=kind, target_mode=mode, target_entity=None,
                             target_position=position, unit_type=None,
                             technology=None, upgrade=None, queue_slot=None,
                             order=None, queued=False))


class WindowCapacityTest(unittest.TestCase):
    def test_multi_command_upper_bounds_are_nested(self):
        row = window_capacity([
            label("right_click", "position", 1, position=[100, 200]),
            label("right_click", "position", 2, position=[100, 200]),
            label("right_click", "position", 3, position=[300, 200]),
            label("train", "none", 4, domain="production"),
        ])
        self.assertEqual(row["commands"], 4)
        self.assertEqual(row["distinct_kinds"], 2)
        self.assertEqual(row["distinct_packets"], 3)
        self.assertEqual(row["oracle_kind_coverage"]["1"], 3)
        self.assertEqual(row["oracle_kind_mode_coverage"]["1"], 3)
        self.assertEqual(row["oracle_packet_coverage"]["1"], 2)
        self.assertEqual(row["oracle_packet_coverage"]["2"], 3)
        self.assertEqual(row["oracle_packet_coverage"]["4"], 4)
        self.assertEqual(row["ordered_packet_runs"], 3)
        self.assertEqual(row["chronological_command_coverage"]["2"], 2)
        self.assertEqual(row["chronological_packet_coverage"]["1"], 2)
        self.assertEqual(row["chronological_packet_coverage"]["2"], 3)
        summary = summarize([row], 2)
        self.assertEqual(summary["multi_domain_windows"], 1)
        self.assertEqual(summary["oracle_coverage"]["packet"]["1"], 2)

    def test_repeated_actor_and_reordered_packets_are_visible(self):
        row = window_capacity([
            label("train", "none", 1, domain="production"),
            label("move", "position", 2, position=[10, 20]),
            label("train", "none", 1, domain="production"),
        ])
        self.assertEqual(row["distinct_packets"], 2)
        self.assertEqual(row["ordered_packet_runs"], 3)
        self.assertEqual(row["repeated_actor_commands"], 1)
        self.assertEqual(row["chronological_packet_coverage"]["2"], 2)

    def test_empty_or_censored_cohorts_are_rejected(self):
        self.assertIsNone(window_capacity([]))
        with self.assertRaises(ValueError):
            summarize([], 1)
        with self.assertRaises(ValueError):
            summarize([window_capacity([label("train", "none", 1)])], 0)


if __name__ == "__main__":
    unittest.main()
