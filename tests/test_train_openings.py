import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('trainer', Path(__file__).resolve().parents[1] / 'tools/train_openings.py')
trainer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trainer)


class TrainingValidation(unittest.TestCase):
    def test_normal_flag_does_not_hide_runtime_forfeit(self):
        rows = self.pair()
        limits = [dict(timeInMS=55, frameCount=320)]
        for row in rows:
            row['timers'] = [dict(timeInMS=55, frameCount=0)]
        rows[1]['timers'][0]['frameCount'] = 320
        with self.assertRaisesRegex(ValueError, 'runtime limit'):
            trainer.validate_pair(rows, limits)
        rows[1]['timers'][0]['frameCount'] = 319
        trainer.validate_pair(rows, limits)
        rows[1]['timers'] = []
        with self.assertRaisesRegex(ValueError, 'missing runtime'):
            trainer.validate_pair(rows, limits)

    def test_cumulative_deduplication(self):
        first = (('Enemy', 'Map', 'standard'), False,
                 dict(run=str(Path('run-a').resolve()), gameID=0, traceSHA256='abc'))
        second = (first[0], True, dict(run=str(Path('run-b').resolve()), gameID=0, traceSHA256='def'))
        self.assertEqual(len(trainer.merge_episodes([first, first, second])), 2)

    def test_conflicting_episode_rejected(self):
        source = dict(run=str(Path('run-a').resolve()), gameID=0, traceSHA256='abc')
        first = (('Enemy', 'Map', 'standard'), False, source)
        for changed in [(first[0], True, source),
                        (first[0], False, dict(source, traceSHA256='changed'))]:
            with self.assertRaises(ValueError):
                trainer.merge_episodes([first, changed])

    def pair(self):
        common = dict(gameID=0, map='Map', gameEndType='NORMAL', crash=False,
                      gameTimeout=False, finalFrame=12000)
        return [dict(common, reportingBot='Protodd', opponentBot='Enemy', won=False),
                dict(common, reportingBot='Enemy', opponentBot='Protodd', won=True)]

    def test_valid_loss(self):
        self.assertFalse(trainer.validate_pair(self.pair())['won'])

    def test_missing_or_duplicate_report(self):
        pair = self.pair()
        for rows in [pair[:1], pair + pair[:1], [pair[0], pair[0]]]:
            with self.assertRaises(ValueError):
                trainer.validate_pair(rows)

    def test_invalid_outcomes(self):
        for field, value in [('crash', True), ('gameTimeout', True),
                             ('gameEndType', 'CRASH'), ('won', False),
                             ('map', 'Other'), ('finalFrame', 400), ('won', 'true')]:
            pair = self.pair()
            pair[1][field] = value
            with self.assertRaises(ValueError):
                trainer.validate_pair(pair)


if __name__ == '__main__':
    unittest.main()
