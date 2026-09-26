import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from training.schema import sha256
from training.worker_outcome_dataset import build, context


STATE = ('STATE,2160,PvT,Hold,Unknown,0,0,50,25,22,34,0,normal,saving,'
         'probes=11,army=2,nexuses=1,gateways=1,zealots=1,dragoons=0,'
         'enemyVisibleArmy=0\n')


class WorkerOutcomeDatasetTests(unittest.TestCase):
    def test_context_must_be_one_pre_intervention_state(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'Protodd.log'
            log.write_text(STATE + STATE)
            with self.assertRaisesRegex(ValueError, 'repeated'):
                context(log)

    def test_paired_outcomes_remain_training_only_and_source_pinned(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            campaigns = {}
            for profile, won in [('baseline', False), ('plus-one', True), ('plus-two', False)]:
                run = root / profile
                run.mkdir()
                (run / 'manifest.json').write_text('{}')
                log = run / 'Protodd.log'
                log.write_text(STATE)
                campaigns[profile] = {
                    'campaign': str(run),
                    'manifest_sha256': sha256(run / 'manifest.json'),
                    'games': [{
                        'game_id': 0, 'opponent': 'UABTerran', 'map': 'Benzene',
                        'match': {'seed': '31'}, 'host': True, 'exposed': profile != 'baseline',
                        'won': won, 'log': str(log), 'log_sha256': sha256(log),
                    }],
                }
            report = root / 'pilot-report.json'
            report.write_text(json.dumps({
                'schema': 'protodd-worker-outcome-pilot-v1',
                'usable_training_outcomes': True,
                'checks': {'healthy': True, 'matched': True,
                           'identical_inputs': True, 'runtime': True},
                'promotion_eligible': False,
                'campaigns': campaigns,
            }))
            with patch('training.worker_outcome_dataset.verify'):
                result = build([report])
            self.assertEqual(result['counts']['examples'], 2)
            self.assertEqual(result['counts']['distinct_contexts'], 1)
            self.assertEqual(result['counts']['positive_differences'], 1)
            self.assertFalse(result['ready_for_bounded_fit'])
            self.assertFalse(result['promotion_eligible'])
            (root / 'plus-one' / 'Protodd.log').write_text(STATE + 'tampered\n')
            with patch('training.worker_outcome_dataset.verify'):
                with self.assertRaisesRegex(ValueError, 'log changed'):
                    build([report])


if __name__ == '__main__':
    unittest.main()
