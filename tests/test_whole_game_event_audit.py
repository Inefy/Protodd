import unittest

from training.whole_game_event_audit import summarize


class EventAuditTests(unittest.TestCase):
    def test_natural_frequency_precision_recall_and_average_precision(self):
        rows = [dict(game_id="g", frame=frame, event=event, probability=probability)
                for frame, event, probability in ((0, 1, 0.9), (24, 0, 0.8),
                                                   (48, 1, 0.7), (72, 0, 0.1))]
        score = summarize(rows)
        self.assertEqual(score["windows"], 4)
        self.assertEqual(score["events"], 2)
        self.assertAlmostEqual(score["average_precision"], (1 + 2 / 3) / 2)
        self.assertAlmostEqual(score["constant_prior_brier"], 0.25)
        self.assertAlmostEqual(score["average_precision_above_prior"], 1 / 3)
        self.assertEqual(score["thresholds"]["0.75"]["predicted"], 2)
        self.assertEqual(score["thresholds"]["0.75"]["true_positive"], 1)
        self.assertAlmostEqual(score["thresholds"]["0.75"]["recall"], 0.5)


if __name__ == "__main__":
    unittest.main()
