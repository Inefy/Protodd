import unittest
import numpy as np
from training.production_goals import goal_rows, GoalLedger
from training.schema import load_schema


def rows(frames, probes):
    schema = load_schema()['features']
    lookup = {f['name']: (i, f['scale']) for i, f in enumerate(schema)}
    x = np.zeros((len(frames), len(schema)), dtype=np.float32)
    i, scale = lookup['own_complete/probe']
    x[:, i] = np.array(probes)/scale
    return dict(frames=np.array(frames), features=x, labels=np.zeros(len(frames), dtype=np.int64),
                action_frames=np.array(frames))


class GoalTargetsTest(unittest.TestCase):
    def test_future_population_is_target_only_and_end_censored(self):
        a = rows([0, 24, 48], [4, 5, 6])
        b = rows([0, 24, 48], [4, 8, 9])
        da, va = goal_rows(a, [], 0, 48, 48)
        db, vb = goal_rows(b, [], 0, 48, 48)
        np.testing.assert_array_equal(da['features'][0], db['features'][0])
        self.assertEqual(da['goals'][0, 0], 6)
        self.assertEqual(db['goals'][0, 0], 9)
        self.assertEqual(va.tolist(), [True, False, False])

    def test_gap_censors_population_target(self):
        _, valid = goal_rows(rows([0, 48, 72], [4, 5, 6]), [], 0, 100, 48)
        self.assertFalse(valid.any())

    def test_active_and_waiting_units_are_distinct_commitments(self):
        r = rows([0, 24, 48], [4, 4, 6])
        schema = load_schema()['features']
        for prefix in ('own_incomplete/', 'own_queue/'):
            i = next(i for i, f in enumerate(schema) if f['name'] == prefix+'probe')
            r['features'][0, i] = 1/schema[i]['scale']
        events = [dict(frame=1, action='train_probe', owner=0, accepted=True, repeated=False)]
        data, _ = goal_rows(r, events, 0, 100, 48)
        self.assertEqual(data['committed'][0, 0], 6)
        self.assertEqual(data['priority'][0], -1)

    def test_priority_ignores_rejections_and_other_owner(self):
        r = rows([0, 24, 48], [4, 5, 6])
        events = [dict(frame=f, action='train_probe', owner=owner, accepted=accepted, repeated=False)
                  for f, owner, accepted in ((1, 1, True), (2, 0, False), (3, 0, True))]
        data, _ = goal_rows(r, events, 0, 100, 48)
        self.assertEqual(data['priority'][0], 0)

    def test_absolute_goal_does_not_multiply_and_recovers_loss(self):
        ledger = GoalLedger()
        self.assertTrue(ledger.observe(0, [22, 2, 3], [19, 2, 3]))
        for frame in (1, 2, 3):
            ledger.accept(frame, 0)
        self.assertFalse(ledger.observe(0, [22, 2, 3], [19, 2, 3]))
        self.assertEqual(ledger.deficits(4).tolist(), [0, 0, 0])
        with self.assertRaises(ValueError):
            ledger.accept(4, 0)
        ledger.observe(240, [22, 2, 3], [18, 2, 3])
        self.assertEqual(ledger.deficits(241).tolist(), [4, 0, 0])
        self.assertEqual(ledger.deficits(1440).tolist(), [0, 0, 0])


if __name__ == '__main__':
    unittest.main()
