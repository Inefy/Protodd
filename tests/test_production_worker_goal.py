import unittest
from training.production_worker_goal import WorkerGoalLedger, gates


class WorkerGoalTest(unittest.TestCase):
    def test_lower_forecast_does_not_erase_recovery_commitment(self):
        ledger = WorkerGoalLedger()
        ledger.propose(0, 22)
        ledger.propose(240, 18)
        self.assertEqual(ledger.deficit(241, 19), 3)
        self.assertEqual(ledger.deficit(241, 17), 5)
        self.assertEqual(ledger.deficit(1200, 17), 1)
        self.assertEqual(ledger.deficit(1440, 17), 0)

    def test_repeating_does_not_extend_expiry_or_multiply_goal(self):
        ledger = WorkerGoalLedger()
        self.assertTrue(ledger.propose(0, 22))
        self.assertFalse(ledger.propose(0, 30))
        self.assertEqual(ledger.deficit(1, 21), 1)
        self.assertEqual(ledger.deficit(2, 22), 0)
        self.assertEqual(ledger.deficit(1200, 21), 0)
        with self.assertRaises(ValueError):
            ledger.propose(24, float('nan'))

    def test_late_failure_cannot_hide_behind_global_recall(self):
        candidate = dict(mae=.5, growth=dict(precision=1., recall=1.),
                         late=dict(positive_rows=100, precision=1., recall=.5),
                         per_game={'one': dict(positive_rows=100, recall=1.)}, excessive_goals=0)
        reference = {k: dict(mae=2.) for k in ('persistence', 'time_race_median')}
        result = gates(candidate, reference)
        self.assertFalse(result['passed'])
        self.assertFalse(result['checks']['late_recall'])


if __name__ == '__main__':
    unittest.main()
