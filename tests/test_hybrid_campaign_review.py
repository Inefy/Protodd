import json
from pathlib import Path
import tempfile
import unittest

from tools.hybrid_campaign_review import review
from training.schema import sha256


class HybridReviewTests(unittest.TestCase):
    def fixture(self, root):
        dll = root / 'model.dll'
        dll.write_bytes(b'fixture DLL')
        (root / 'ProtoddEvaluation.build.json').write_text(json.dumps(dict(dll_sha256=sha256(dll))))
        for arm in ('shadow', 'target'):
            run = root / arm
            server = run / 'server'
            server.mkdir(parents=True)
            (server / 'server_settings.json').write_text(json.dumps(dict(
                serverPort=1571 if arm == 'shadow' else 1573,
                tournamentModuleSettings=dict(gameFrameLimit=86400,
                    timeoutLimits=[dict(timeInMS=55, frameCount=320)]))))
            schedule, reports = [], []
            for gid in range(2):
                host = gid == 0
                schedule.append(dict(gameID=gid, homeBot='Protodd' if host else 'BananaBrain',
                                     awayBot='BananaBrain' if host else 'Protodd', map='Benzene'))
                for bot, opponent in [('Protodd', 'BananaBrain'), ('BananaBrain', 'Protodd')]:
                    reports.append(dict(gameID=gid, reportingBot=bot, opponentBot=opponent,
                        wasHost=host if bot == 'Protodd' else not host, map='Benzene',
                        gameEndType='NORMAL', won=bot == 'BananaBrain', crash=False,
                        gameTimeout=False, finalFrame=1000, timers=[dict(timeInMS=55, frameCount=0)]))
                received = server / f'replays/bot-write/game-{gid}/Protodd/received'
                received.mkdir(parents=True)
                (received / 'Protodd.log').write_text(
                    f'CONTROLLER,whole-game,weights=1,control=1,mode=hybrid,hybridControl={int(arm == "target")}\n'
                    f'MATCH,seed={42 + gid},map_hash=frozen,width=4096,height=3584\n'
                    'HYBRID_SUMMARY,received=4,targets=0,submitted=0,accepted=0\n'
                    'PERF_SUMMARY,1001,1,20,0,0,0,0\n')
                (received / 'WholeGame-inference.csv').write_text('7,10,0,10,-1,-1\n')
                (received / 'WholeGame-intents.csv').write_text('7,0,train_worker\n')
                entities = [dict(type=64, relation=0) for _ in range(6)] + [dict(type=160, relation=0)]
                (received / 'WholeGame-observations.jsonl').write_text(json.dumps(
                    dict(frame=7, minerals=50, entities=entities)) + '\n')
                opponent = server / f'replays/bot-write/game-{gid}/BananaBrain/received'
                opponent.mkdir(parents=True)
                (opponent / 'Results_Protodd.txt').write_text('a,b,c,d,e,PvP_3gaterobo\n')
            (server / 'games.jsonl').write_text(''.join(json.dumps(g) + '\n' for g in schedule))
            (server / 'results.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in reports))
            target = server / 'bots/Protodd/AI/Protodd.dll'
            target.parent.mkdir(parents=True)
            target.write_bytes(dll.read_bytes())
            (run / 'manifest.json').write_text(json.dumps(dict(format='protodd-arena-v1',
                complete=True, bot='Protodd', games=2, purpose='development',
                components={'server/bots/Protodd/AI/Protodd.dll': sha256(target)})))

    def test_zero_exposure_is_not_learned_strength(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            report = review(root)
            self.assertTrue(report['matched_actual_maps_and_seeds'])
            self.assertTrue(report['strict_runtime_gate_pass'])
            self.assertEqual(report['learned_target_exposure'], 0)
            self.assertFalse(report['strength_validated'])
            self.assertIn('cannot be attributed', report['interpretation'])

    def test_wrong_model_authority_or_host_is_rejected(self):
        for old, new in [('weights=1', 'weights=0'), ('hybridControl=1', 'hybridControl=0')]:
            with self.subTest(new=new), tempfile.TemporaryDirectory() as temp:
                root = Path(temp)
                self.fixture(root)
                log = root / 'target/server/replays/bot-write/game-0/Protodd/received/Protodd.log'
                log.write_text(log.read_text().replace(old, new))
                with self.assertRaisesRegex(ValueError, 'wrong hybrid authority'):
                    review(root)
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            path = root / 'target/server/results.jsonl'
            reports = [json.loads(line) for line in path.read_text().splitlines()]
            reports[0]['wasHost'] = False
            path.write_text(''.join(json.dumps(r) + '\n' for r in reports))
            with self.assertRaisesRegex(ValueError, 'actual host'):
                review(root)

    def test_unmatched_seeds_are_rejected(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            self.fixture(root)
            log = root / 'target/server/replays/bot-write/game-0/Protodd/received/Protodd.log'
            log.write_text(log.read_text().replace('seed=42', 'seed=99'))
            with self.assertRaisesRegex(ValueError, 'actual seeds/maps'):
                review(root)


if __name__ == '__main__':
    unittest.main()
