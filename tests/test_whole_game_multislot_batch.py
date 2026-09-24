import unittest

import torch
from torch.nn import functional as F

from training.whole_game_model import KINDS, TARGET_MODES
from training.whole_game_multislot_batch import conditioned_action_loss


class ConditionedActionLossTest(unittest.TestCase):
    def test_mode_legality_uses_teacher_kind(self):
        kind = torch.zeros((1, len(KINDS)), requires_grad=True)
        mode = torch.tensor([[4.0, 0.0, -2.0]], requires_grad=True)
        label = dict(kind=torch.tensor([KINDS.index("train")]),
                     target_mode=torch.tensor([TARGET_MODES.index("none")]),
                     mask=dict(kind=True, target_mode=True))
        loss = conditioned_action_loss(dict(kind=kind, target_mode=mode), label)
        baseline = (F.cross_entropy(kind, label["kind"]) +
                    F.cross_entropy(mode, label["target_mode"]))
        self.assertGreater(float(loss.detach()), float(baseline.detach()))
        loss.backward()
        self.assertGreater(mode.grad.abs().sum().item(), 0)

    def test_all_modes_legal_adds_no_penalty(self):
        kind = torch.zeros((1, len(KINDS)))
        mode = torch.tensor([[1.0, 2.0, 3.0]])
        label = dict(kind=torch.tensor([KINDS.index("cast")]),
                     target_mode=torch.tensor([TARGET_MODES.index("position")]),
                     mask=dict(kind=True, target_mode=True))
        loss = conditioned_action_loss(dict(kind=kind, target_mode=mode), label)
        baseline = (F.cross_entropy(kind, label["kind"]) +
                    F.cross_entropy(mode, label["target_mode"]))
        self.assertAlmostEqual(float(loss), float(baseline), places=6)


if __name__ == "__main__":
    unittest.main()
