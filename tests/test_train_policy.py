import sys
import json
from pathlib import Path
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tools'))
from train_policy import transitions, read_episode


class PolicyDatasetTests(unittest.TestCase):
    def test_evaluation_campaign_rejected_before_trace_access(self):
        with tempfile.TemporaryDirectory() as temp:
            run = Path(temp)
            (run / 'manifest.json').write_text(json.dumps(dict(format='protodd-arena-v1', purpose='development')))
            with self.assertRaisesRegex(ValueError, 'campaign purpose'):
                read_episode(run, 0)

    def test_episode_return_credits_early_actions_once_without_stall_bonus(self):
        trace = self.trace().replace('END,1000,1', 'DECISION,960,1,0,15\nEND,2000,0')
        rows = transitions(trace, dict(won=False, finalFrame=2000), 'Terran', 'episode-return')
        self.assertEqual(len(rows), 2)
        self.assertIn('Terran_Protoss_v1 1 0 -1 0 0 1', rows)
        self.assertIn('Terran_Protoss_v1 2 3 -1 0 0 1', rows)
        longer = trace.replace('END,2000,0', 'END,3000,0')
        self.assertEqual(rows, transitions(longer, dict(won=False, finalFrame=3000), 'Terran', 'episode-return'))

    def trace(self):
        return 'BEGIN,1,Terran,Protoss,Terran_Protoss_v1,123,train\nDECISION,0,1,0,15\nDECISION,480,2,3,8\nEND,1000,1\n'

    def test_terminal_reward_and_reverse_replay(self):
        rows = transitions(self.trace(), dict(won=True, finalFrame=1000), 'Terran')
        self.assertEqual(rows, ['Terran_Protoss_v1 2 3 1 0 0 1', 'Terran_Protoss_v1 1 0 0 2 8 0'])

    def test_reject_invalid_and_heldout(self):
        for trace in [self.trace().replace(',train', ',frozen'),
                      self.trace().replace('480,2,3,8', '480,2,2,8'),
                      self.trace().replace('480,2', '0,2'),
                      self.trace().replace('END,1000,1', 'END,1000,0')]:
            with self.assertRaises(ValueError):
                transitions(trace, dict(won=True, finalFrame=1000), 'Terran')

    def test_race_isolation(self):
        with self.assertRaises(ValueError):
            transitions(self.trace(), dict(won=True, finalFrame=1000), 'Zerg')


if __name__ == '__main__':
    unittest.main()
