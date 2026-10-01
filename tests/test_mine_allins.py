import unittest

from training.mine_allins import classify, summarize, wilson
from training.mine_allin_requests import request_profile


class AllinMiningTests(unittest.TestCase):
    def test_raw_request_repeats_and_expansion_exclude_false_commitment(self):
        def build(unit, frame, x):
            return dict(PlayerID=1, Frame=frame, Type=dict(Name='Build'), Unit=dict(ID=unit), Pos=dict(X=x, Y=4))
        commands = [build(160, 1800, 4), build(160, 1850, 4), build(160, 2400, 8)]
        result = request_profile(commands, dict(ID=1))
        self.assertEqual(result['family'], 'gasless-two-gate-zealot')
        self.assertEqual(result['first']['gateway_2'], 2400)
        self.assertNotIn('gateway_3', result['first'])
        self.assertIsNone(request_profile(commands + [build(154, 2800, 24)], dict(ID=1)))
        self.assertIsNone(request_profile(commands, dict(ID=2)))

    def test_raw_requests_distinguish_gateway_ceiling_and_dt(self):
        def command(unit, frame, x=0, name='Build'):
            return dict(PlayerID=1, Frame=frame, Type=dict(Name=name), Unit=dict(ID=unit), Pos=dict(X=x, Y=4))
        commands = [command(160, 1800, 4), command(157, 2200), command(164, 2800),
                    command(66, 4000, name='Train'), command(160, 5200, 8), command(160, 5900, 12)]
        self.assertEqual(request_profile(commands, dict(ID=1))['family'], 'three-gate-dragoon')
        self.assertEqual(request_profile(commands + [command(160, 6900, 16)], dict(ID=1))['family'], 'four-gate-dragoon')
        self.assertEqual(request_profile(commands + [command(165, 5800)], dict(ID=1))['family'], 'one-base-dt')

    def test_gasless_commitment_does_not_require_surviving_army(self):
        first = dict(gateway_2=3000, assimilator_1=5000, cybernetics_core_1=5500)
        self.assertEqual(classify(first, lambda _: 14), ('gasless-two-gate-zealot', 3000))
        first['nexus_2'] = 3200
        self.assertIsNone(classify(first, lambda _: 14))

    def test_distinct_tech_and_gateway_commitments(self):
        for first, family in [
            (dict(dark_templar_1=8000), 'one-base-dt'),
            (dict(reaver_1=9500), 'one-base-reaver'),
            (dict(gateway_4=7000, dragoon_2=6500), 'four-gate-dragoon'),
            (dict(gateway_3=6500, dragoon_2=6200), 'three-gate-dragoon'),
            (dict(gateway_4=7000, zealot_2=4500), 'four-gate-zealot'),
        ]:
            with self.subTest(family=family):
                self.assertEqual(classify(first, lambda _: 20)[0], family)
                self.assertIsNone(classify(dict(first, nexus_2=1000), lambda _: 20))
                self.assertIsNone(classify(first, lambda _: 40))

    def test_unknown_outcomes_are_not_losses(self):
        base = dict(matchup='PvP', family='one-base-dt', game_id='g', path='g.rep', perspective=0,
                    first={}, ready_frame=8000, commitment_frame=8000, workers_at_commitment=20,
                    forward_attack_frame=None)
        rows = [dict(base, won=True, outcome_source='quit'),
                dict(base, game_id='h', won=None, outcome_source='unknown')]
        group = summarize(rows)[0]
        self.assertEqual(group['perspectives'], 2)
        self.assertEqual(group['known_outcomes'], 1)
        self.assertEqual(group['wins'], 1)
        self.assertEqual(group['inferred_or_claimed_win_rate'], 1)
        self.assertLess(group['wilson_95'][0], 0.3)
        self.assertEqual(wilson(0, 0), [0, 1])


if __name__ == '__main__':
    unittest.main()
