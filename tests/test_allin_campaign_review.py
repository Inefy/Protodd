import json
import hashlib
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import zipfile

from tools.allin_campaign_review import opening_observations, banana_evidence, pluto_evidence
from tools.prepare_allin_opponents import prepare as prepare_opponents


class AllinCampaignReviewTests(unittest.TestCase):
    def test_opponent_staging_pins_components_and_ignores_arbitrary_zip_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            template = root / 'template'
            for name in ('client1', 'client2', 'server/required', 'server/html'):
                (template / name).mkdir(parents=True)
            (template / 'server/server_settings.json').write_text('{}')
            (template / 'server/server.jar').write_bytes(b'jar')
            banana = root / 'banana'
            banana.mkdir()
            (banana / 'Configuration.txt').write_text('tournament=true\n')
            (banana / 'BananaBrain.dll').write_bytes(b'banana')
            package = root / 'pluto.zip'
            files = {'pluto.dll': b'dll', 'pluto/pluto_infer.exe': b'engine', 'pluto/pluto_weights.bin': b'weights'}
            with zipfile.ZipFile(package, 'w') as archive:
                for name, data in files.items():
                    archive.writestr(name, data)
                archive.writestr('../outside.dll', b'not allowed')
            digest = lambda data: hashlib.sha256(data).hexdigest()
            with patch('tools.prepare_allin_opponents.PLUTO_ZIP_SHA', digest(package.read_bytes())), \
                    patch('tools.prepare_allin_opponents.PLUTO_FILES', {k: digest(v) for k, v in files.items()}):
                prepare_opponents(template, banana, package, root / 'output')
            ai = root / 'output/server/bots/Pluto/AI'
            self.assertEqual((ai / 'Pluto.dll').read_bytes(), b'dll')
            self.assertEqual((ai / 'pluto/pluto_weights.bin').read_bytes(), b'weights')
            self.assertFalse(any((root / 'output/server/bots/BananaBrain/read').iterdir()))
            self.assertFalse(list(root.rglob('outside.dll')))
            settings = json.loads((root / 'output/server/server_settings.json').read_text())
            self.assertEqual([b['Race'] for b in settings['bots']], ['Protoss', 'Protoss'])

    def test_wrong_pluto_package_is_rejected_before_output_creation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            package = root / 'wrong.zip'
            package.write_bytes(b'wrong')
            with self.assertRaisesRegex(ValueError, 'unrecognized Pluto package'):
                prepare_opponents(root / 'missing-template', root / 'missing-banana', package, root / 'output')
            self.assertFalse((root / 'output').exists())

    def test_stock_banana_does_not_assume_three_gate_robo(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            received = root / 'server/replays/bot-write/game-0/BananaBrain/received'
            received.mkdir(parents=True)
            (received / 'Results_Protodd.txt').write_text('date,2,8,2,map,PvP_3gatespeedzeal,none,P_4gate,10000,0,-1,-1,1\n')
            result = banana_evidence(root, 0)
            self.assertEqual(result['opening'], 'PvP_3gatespeedzeal')
            self.assertIsNone(result['frozen_opening'])
            with self.assertRaisesRegex(ValueError, 'differs from paired result'):
                banana_evidence(root, 0, dict(frame=10000, won=True), 'map')
            config = root / 'server/bots/BananaBrain/read/Configuration.txt'
            config.parent.mkdir(parents=True)
            config.write_text('PvP_opening=PvP_3gaterobo\n')
            with self.assertRaisesRegex(ValueError, 'changed its frozen opening'):
                banana_evidence(root, 0)

    def test_pluto_inference_error_cannot_count_as_a_win(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            received = root / 'server/replays/bot-write/game-0/Pluto/received'
            received.mkdir(parents=True)
            start = dict(t='start', id=1, opp='Protodd', map='map', own_race='P', opp_race='P',
                         mode='block', budget_ms=40, model='md07x02_cog2026_2578600_int8mv', fps=6, bo='DTRush')
            end = dict(t='end', id=1, opp='Protodd', result='loss', frames=10000, steps=1667)
            record = received / 'pluto_bandit_Protodd.txt'
            record.write_text(json.dumps(start) + '\n' + json.dumps(end) + '\nend\n')
            (received / 'pluto.log').write_text('inference server up\nonEnd\n')
            (received / 'pluto_infer.log').write_text('[pluto_infer] ready:\n')
            outcome = dict(won=True, frame=10000)
            self.assertEqual(pluto_evidence(root, 0, outcome, 'map')['opening'], 'DTRush')
            (received / 'pluto.log').write_text('inference server up\nonEnd\nquitting in 10 seconds\n')
            with self.assertRaisesRegex(ValueError, 'engine did not finish healthy'):
                pluto_evidence(root, 0, outcome, 'map')
            (received / 'pluto.log').write_text('inference server up\nonEnd\n')
            record.write_text(json.dumps(start) + '\n' + json.dumps(dict(t='event', what='engine_error')) +
                              '\n' + json.dumps(end) + '\nend\n')
            with self.assertRaisesRegex(ValueError, 'engine error/event'):
                pluto_evidence(root, 0, outcome, 'map')

    def test_completed_counts_and_forward_motion_are_not_command_claims(self):
        def entity(kind, complete, position, relation=0):
            return dict(type=kind, completed=complete, position=position, relation=relation)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'observations.jsonl'
            rows = [dict(frame=7, entities=[entity(154, 1, [100, 100])]),
                    dict(frame=31, entities=[entity(65, 0, [1600, 100])] * 4),
                    dict(frame=55, entities=[entity(65, 1, [1600, 100], 1)] * 4),
                    dict(frame=79, entities=[entity(65, 1, [300, 100])] * 4),
                    dict(frame=103, entities=[entity(65, 1, [1600, 100])] * 4)]
            path.write_text(''.join(json.dumps(row) + '\n' for row in rows))
            result = opening_observations(path)
            self.assertEqual(result['first_completed']['65_4'], 79)
            self.assertEqual(result['max_completed'][65], 4)
            self.assertEqual(result['first_four_fighters_1200_from_home'], 103)


if __name__ == '__main__':
    unittest.main()
