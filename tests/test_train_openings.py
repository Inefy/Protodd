import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('trainer', Path(__file__).resolve().parents[1] / 'tools/train_openings.py')
trainer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(trainer)


class TrainingValidation(unittest.TestCase):
    def test_evaluation_purpose_rejected_before_reading_results(self):
        with tempfile.TemporaryDirectory() as temp:
            run = Path(temp)
            for purpose in ('development', 'final-test', None):
                (run / 'manifest.json').write_text(json.dumps(dict(format='protodd-arena-v1', purpose=purpose)))
                with self.assertRaisesRegex(ValueError, 'campaign purpose'):
                    trainer.read_episode(run, 0)
            (run / 'manifest.json').write_text(json.dumps(dict(format='protodd-arena-v1', purpose='training')))
            trainer.require_training_campaign(run)

    def test_opening_trace_must_explicitly_be_a_training_episode(self):
        with tempfile.TemporaryDirectory() as temp:
            run = Path(temp)
            trace_dir = run / 'server/replays/bot-write/game-0/Protodd/archive'
            trace_dir.mkdir(parents=True)
            (run / 'manifest.json').write_text(json.dumps(dict(format='protodd-arena-v1', purpose='training')))
            (run / 'server/results.jsonl').write_text('\n'.join(map(json.dumps, self.pair())) + '\n')
            (run / 'server/server_settings.json').write_text(json.dumps(dict(tournamentModuleSettings=dict(timeoutLimits=[]))))
            trace = trace_dir / 'Protodd.log'
            episode = 'START,Map,Enemy,standard\nEND,loss,12000\n'
            for mode in ('', 'LEARNING,mode=frozen\n'):
                trace.write_text(mode + episode)
                with self.assertRaisesRegex(ValueError, 'opening-learning'):
                    trainer.read_episode(run, 0)
            trace.write_text('LEARNING,mode=validated-train\n' + episode)
            self.assertFalse(trainer.read_episode(run, 0)[1])

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
