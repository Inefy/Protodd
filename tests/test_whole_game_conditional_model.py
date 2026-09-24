import tempfile
import unittest
from pathlib import Path

import torch
from torch.nn import functional as F

from training.whole_game_conditional_model import (ConditionalWholeGameModel,
                                                    initialize_from_baseline)
from training.whole_game_model import WholeGameModel


class ConditionalWholeGameModelTests(unittest.TestCase):
    def test_zero_residual_preserves_baseline_and_trains_conditional_heads(self):
        torch.manual_seed(9)
        base = WholeGameModel(width=64, mixture_components=3).eval()
        model = ConditionalWholeGameModel(width=64, mixture_components=3).eval()
        model.load_state_dict(base.state_dict(), strict=False)
        batch = dict(type=torch.tensor([[64, 65, 66]]),
                     relation=torch.tensor([[0, 1, 0]]),
                     order=torch.tensor([[0, 0, 0]]),
                     entity_numeric=torch.zeros((1, 3, 16)),
                     entity_mask=torch.ones((1, 3), dtype=torch.bool),
                     spatial=torch.zeros((1, 3, 8, 8)),
                     global_=torch.zeros((1, 216)))
        batch["global"] = batch.pop("global_")
        baseline = base(batch)
        output = model(batch)
        for name in ("kind", "target_mode", "unit_type", "target", "position"):
            self.assertTrue(torch.allclose(output[name], baseline[name], atol=1e-6), name)
        self.assertTrue(torch.isneginf(output["actor"][0, 1]))
        self.assertTrue(torch.isfinite(output["actor"][0, [0, 2]]).all())
        loss = (F.cross_entropy(output["kind"], torch.tensor([12])) +
                F.cross_entropy(output["target_mode"], torch.tensor([2])) +
                F.cross_entropy(output["unit_type"], torch.tensor([64])) +
                output["position"].square().mean())
        loss.backward()
        for layer in (model.conditional_kind, model.conditional_mode,
                      model.conditional_unit_type, model.conditional_position):
            self.assertGreater(layer.weight.grad.abs().sum().item(), 0)

    def test_baseline_checkpoint_identity_is_required(self):
        model = ConditionalWholeGameModel(width=64, mixture_components=3)
        base = WholeGameModel(width=64, mixture_components=3)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "baseline.pt"
            torch.save(dict(source_identity_sha256="train-id",
                            state_dict=base.state_dict()), path)
            self.assertEqual(len(initialize_from_baseline(model, path, "train-id")), 64)
            with self.assertRaisesRegex(ValueError, "another replay release"):
                initialize_from_baseline(model, path, "wrong-id")


if __name__ == "__main__":
    unittest.main()
