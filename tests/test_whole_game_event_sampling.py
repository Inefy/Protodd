import unittest

from training.whole_game_event_sampling import natural_event_schedule


class EventSamplingTests(unittest.TestCase):
    def test_natural_class_mix_preserves_equal_matchup_budget(self):
        counts = {matchup: dict(windows=100, events=80)
                  for matchup in ("PvT", "PvZ", "PvP")}
        categories = [(matchup, "event", str(label))
                      for matchup in counts for label in (0, 1)]
        schedule = natural_event_schedule(categories, counts, 60, seed=42)
        self.assertEqual(len(schedule), 60)
        for matchup in counts:
            self.assertEqual(schedule.count((matchup, "event", "1")), 16)
            self.assertEqual(schedule.count((matchup, "event", "0")), 4)
        self.assertEqual(schedule, natural_event_schedule(categories, counts, 60, seed=42))


if __name__ == "__main__":
    unittest.main()
