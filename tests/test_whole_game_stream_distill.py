import unittest
from collections import Counter

from training.whole_game_stream_distill import category_schedule


class StreamDistillTests(unittest.TestCase):
    def test_schedule_preserves_cadence_action_and_natural_event_mix(self):
        buckets = {
            ("PvT", "economy", "right_click"): [1],
            ("PvT", "production", "train"): [1],
            ("PvT", "event", "0"): [1],
            ("PvT", "event", "1"): [1],
            ("PvT", "forecast", "24", "0000"): [1],
        }
        cadence = dict(first_kind_counts=Counter({("PvT", "right_click"): 90,
                                                   ("PvT", "train"): 10}),
                       event_counts={"PvT": Counter(windows=100, events=75)})
        group = {"PvT": ["one"], "PvZ": ["two"], "PvP": ["three"]}
        first = category_schedule(buckets, cadence, group, 16)
        second = category_schedule(buckets, cadence, group, 16)
        self.assertEqual(first, second)
        self.assertEqual(len(first["action"]), 8)
        self.assertEqual(len(first["event"]), 4)
        self.assertEqual(len(first["forecast"]), 1)
        self.assertIn(("PvT", "production", "train"), first["action"])
        self.assertGreater(first["event"].count(("PvT", "event", "1")),
                           first["event"].count(("PvT", "event", "0")))


if __name__ == "__main__":
    unittest.main()
