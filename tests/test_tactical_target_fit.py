import gzip
import json
from pathlib import Path
import tempfile
import unittest

from training.tactical_target_fit import examples_from_shard, features


class TacticalTargetFitTests(unittest.TestCase):
    def test_causal_enemy_target_and_combat_actor_filter(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            actor = dict(id=1, type=66, relation=0, position=[100, 100], hp=100,
                         shields=80, visible=1, own_state=dict(order_target=-1))
            enemy_a = dict(id=2, type=7, relation=1, position=[200, 100], hp=40,
                           shields=0, visible=1)
            enemy_b = dict(id=3, type=8, relation=1, position=[220, 100], hp=80,
                           shields=0, visible=1)
            observation = dict(sequence=9, frame=120, reason="before_command",
                               entities=[actor, enemy_a, enemy_b])
            label = dict(observation_sequence=9, frame=120, domain="unit_control",
                         actions=dict(kind="attack", target_mode="entity", target_entity=3),
                         loss_masks=dict(target_entity=True), actor_positive=[1])
            with gzip.open(directory / "observations.jsonl.gz", "wt") as stream:
                stream.write(json.dumps(observation) + "\n")
            with gzip.open(directory / "imitation-labels.jsonl.gz", "wt") as stream:
                stream.write(json.dumps(label) + "\n")
            examples = examples_from_shard(directory, 10)
            self.assertEqual(len(examples), 1)
            values, types, target_index = examples[0]
            self.assertEqual(types.tolist(), [7, 8])
            self.assertEqual(target_index, 1)
            self.assertAlmostEqual(values[0, 0], 100 / 640)
            self.assertEqual(values[0, 4], 1 / 8)
            label["actions"]["kind"] = "right_click"
            with gzip.open(directory / "imitation-labels.jsonl.gz", "wt") as stream:
                stream.write(json.dumps(label) + "\n")
            self.assertEqual(examples_from_shard(directory, 10), [])
            self.assertEqual(len(examples_from_shard(directory, 10, True)), 1)
            label["actions"]["target_entity"] = 1
            with gzip.open(directory / "imitation-labels.jsonl.gz", "wt") as stream:
                stream.write(json.dumps(label) + "\n")
            self.assertEqual(examples_from_shard(directory, 10, True), [])

    def test_feature_formula(self):
        actor = dict(id=1, position=[0, 0], own_state=dict(order_target=2))
        target = dict(id=2, position=[0, 640], hp=99, shields=0)
        neighbor = dict(id=3, position=[0, 650], hp=20, shields=0)
        result = features(actor, target, [target, neighbor])
        self.assertEqual(result[0], 1)
        self.assertEqual(result[2], 0)
        self.assertEqual(result[3], 1)
        self.assertEqual(result[4], 1 / 8)


if __name__ == "__main__":
    unittest.main()
