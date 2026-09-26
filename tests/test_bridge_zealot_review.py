from pathlib import Path
import tempfile
import unittest

from training.bridge_zealot_review import trace


class BridgeZealotTraceTests(unittest.TestCase):
    def test_observed_enemy_and_actual_train_command_are_required(self):
        with tempfile.TemporaryDirectory() as temp:
            log = Path(temp) / 'Protodd.log'
            log.write_text(
                'MATCH,seed=123,map_hash=abc\n'
                'STATE,6000,x,x,x,0,0,150,70,40,50,0,normal,x,'
                'probes=18,army=3,enemyVisibleArmy=2\n'
                'MACRO,4320,1,Zealot,0,saving-resources,0,'
                'bridge Core warp-in with a second defender,100,0,0,0\n'
                'MACRO,4440,1,Zealot,0,issued-Zealot,1,'
                'bridge Core warp-in with a second defender,100,0,1,1\n'
                'MACRO,5000,1,Dragoon,0,issued-Dragoon,1,first ranged unit,125,50,1,1\n'
            )
            row = trace(log)
            self.assertEqual(row['bridge_orders'], 1)
            self.assertEqual(row['first_dragoon'], 5000)
            self.assertEqual(row['state_6000']['army'], 3)
            self.assertTrue(row['enemy_activity'])


if __name__ == '__main__':
    unittest.main()
