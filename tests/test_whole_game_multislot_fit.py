import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import torch

from training.whole_game_model import WholeGameModel
from training.whole_game_multislot_fit import _initialize
from training.whole_game_multislot_model import MultiSlotWholeGameModel
from training.whole_game_multislot_fit import SCHEMA
from training.whole_game_multislot_collect import collect_multislot_windows


class MultiSlotInitializationTest(unittest.TestCase):
    def test_continue_multislot_preserves_all_trained_tensors(self):
        torch.manual_seed(17)
        previous = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                           maximum_slots=6)
        candidate = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                            maximum_slots=6)
        with tempfile.TemporaryDirectory() as directory:
            checkpoint = Path(directory) / "teacher.pt"
            torch.save(dict(schema=SCHEMA, source_identity_sha256="identity",
                            state_dict=previous.state_dict()), checkpoint)
            _initialize(candidate, checkpoint, "identity")
            for name, value in previous.state_dict().items():
                self.assertTrue(torch.equal(value, candidate.state_dict()[name]), name)
            with self.assertRaisesRegex(ValueError, "another replay release"):
                _initialize(candidate, checkpoint, "other")

    def test_early_scope_rejects_invalid_bound(self):
        for bound in (0, -1, 23, 24.0, True):
            with self.assertRaisesRegex(ValueError, "maximum frame"):
                collect_multislot_windows("unused", "train",
                                         dict(PvP=[], PvT=[], PvZ=[]), maximum_frame=bound)

    def test_early_scope_filters_both_training_and_validation(self):
        group = {matchup: [matchup] for matchup in ("PvP", "PvT", "PvZ")}
        shards = [(dict(matchup=matchup, game_id=matchup), Path(matchup), None)
                  for matchup in group]
        sequences = [dict(observation=dict(frame=frame), context=[], labels=[])
                     for frame in (24, 3600, 3624)]
        for split in ("train", "validation"):
            with patch("training.whole_game_multislot_collect.selected_shards", return_value=shards), \
                 patch("training.whole_game_multislot_collect.load_terrain"), \
                 patch("training.whole_game_multislot_collect.static_grid", return_value=None), \
                 patch("training.whole_game_multislot_collect.trajectory_shard"), \
                 patch("training.whole_game_multislot_collect.cadence_sequences", return_value=sequences):
                buckets, info = collect_multislot_windows(
                    "unused", split, group, maximum_frame=3600, per_game_category_limit=12)
                self.assertEqual(sum(info["windows"].values()), 6)
                self.assertEqual({sample[1]["frame"] for samples in buckets.values()
                                  for sample in samples}, {24, 3600})

    def test_first_slot_preserves_baseline_heads(self):
        torch.manual_seed(7)
        baseline = WholeGameModel(width=64, mixture_components=3).eval()
        model = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                        maximum_slots=2).eval()
        with tempfile.TemporaryDirectory() as directory:
            checkpoint = Path(directory) / "teacher.pt"
            torch.save(dict(source_identity_sha256="train-identity",
                            state_dict=baseline.state_dict()), checkpoint)
            _initialize(model, checkpoint, "train-identity")
            with self.assertRaisesRegex(ValueError, "another replay release"):
                _initialize(model, checkpoint, "other")
        sample = dict(type=torch.tensor([[64, 65, 66]]),
                      relation=torch.tensor([[0, 1, 0]]),
                      order=torch.zeros((1, 3), dtype=torch.long),
                      entity_numeric=torch.zeros((1, 3, 16)),
                      entity_mask=torch.ones((1, 3), dtype=torch.bool),
                      spatial=torch.zeros((1, 3, 8, 8)))
        sample["global"] = torch.zeros((1, 216))
        previous = baseline(sample)
        first = model.forward_slots(sample, slots=1)["slots"][0]
        for name in ("kind", "target_mode", "domain", "queued", "position",
                     "unit_type", "technology", "upgrade", "queue_slot", "order"):
            self.assertTrue(torch.allclose(first[name], previous[name], atol=1e-6), name)
        self.assertTrue(torch.allclose(first["actor"][0, [0, 2]],
                                        previous["actor"][0, [0, 2]], atol=1e-6))
        self.assertTrue(torch.allclose(first["target"][0, [0, 2]],
                                        previous["target"][0, [0, 2]], atol=1e-6))
        self.assertTrue(torch.allclose(first["stop"][0, 0],
                                        previous["event"][0], atol=1e-6))
        self.assertEqual(float(first["stop"][0, 1]), 0.0)


if __name__ == "__main__":
    unittest.main()
