import tempfile
import unittest
from pathlib import Path

import torch

from training.whole_game_model import WholeGameModel
from training.whole_game_multislot_fit import _initialize
from training.whole_game_multislot_model import MultiSlotWholeGameModel


class MultiSlotInitializationTest(unittest.TestCase):
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
