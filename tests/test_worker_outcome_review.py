from pathlib import Path
import tempfile
import unittest

from training.worker_outcome_review import read_game


class WorkerOutcomeReviewTests(unittest.TestCase):
    def test_unexposed_treatment_is_recorded_as_unexposed(self):
        with tempfile.TemporaryDirectory() as temp:
            log=Path(temp)/'Protodd.log'
            log.write_text('MATCH,seed=123,map_hash=abc,width=4096,height=3584\n'
                           'WORKER_TRAINING_MODE,plus-one,enabled=1\n'
                           'STATE,3600,x,Hold,x,0,0,50,0,20,30,0,normal,x,probes=15,army=1,enemyVisibleArmy=2\n')
            row=read_game(log,'plus-one')
            self.assertFalse(row['exposed'])
            self.assertTrue(row['enemy_activity'])

    def test_invalid_scope_and_mode_cannot_be_used_as_training_label(self):
        with tempfile.TemporaryDirectory() as temp:
            log=Path(temp)/'Protodd.log'
            log.write_text('MATCH,seed=123,map_hash=abc,width=4096,height=3584\n'
                           'WORKER_TRAINING_MODE,plus-one,enabled=1\n'
                           'WORKER_TRAINING,7200,committed=19,before=22,goal=23,beforePriority=93,priority=100\n')
            with self.assertRaisesRegex(ValueError,'bounds'):
                read_game(log,'plus-one')
            with self.assertRaisesRegex(ValueError,'identity'):
                read_game(log,'plus-two')


if __name__ == '__main__':
    unittest.main()
