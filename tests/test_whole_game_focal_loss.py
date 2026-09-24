import unittest

import torch
from torch.nn import functional as F

from training.whole_game_focal_loss import focal_structured_action_loss


class FocalActionTests(unittest.TestCase):
    def test_easy_majority_example_is_downweighted_without_masking_hard_kind(self):
        label = dict(mask={"kind": True}, kind=torch.tensor([0]))
        easy = {"kind": torch.tensor([[8.0, -8.0]], requires_grad=True)}
        hard = {"kind": torch.tensor([[-8.0, 8.0]], requires_grad=True)}
        easy_loss, _ = focal_structured_action_loss(easy, label)
        hard_loss, _ = focal_structured_action_loss(hard, label)
        easy_ce = F.cross_entropy(easy["kind"], label["kind"])
        hard_ce = F.cross_entropy(hard["kind"], label["kind"])
        self.assertLess(float(easy_loss / easy_ce), 0.01)
        self.assertGreater(float(hard_loss / hard_ce), 0.99)
        (easy_loss + hard_loss).backward()
        self.assertTrue(torch.isfinite(easy["kind"].grad).all())
        self.assertTrue(torch.isfinite(hard["kind"].grad).all())


if __name__ == "__main__":
    unittest.main()
