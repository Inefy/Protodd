import unittest

import torch
from torch.nn import functional as F

from training.whole_game_model import KINDS, WholeGameModel
from training.whole_game_multislot_model import MultiSlotWholeGameModel


def batch():
    sample = dict(type=torch.tensor([[64, 65, 66]]),
                  relation=torch.tensor([[0, 1, 0]]),
                  order=torch.zeros((1, 3), dtype=torch.long),
                  entity_numeric=torch.zeros((1, 3, 16)),
                  entity_mask=torch.ones((1, 3), dtype=torch.bool),
                  spatial=torch.zeros((1, 3, 8, 8)))
    sample["entity_numeric"][0, 1, 4] = 0  # remembered enemy is not a legal target
    sample["global"] = torch.zeros((1, 216))
    return sample


def token(actor, kind, delay):
    return dict(actor=torch.tensor([actor]), kind=torch.tensor([KINDS.index(kind)]),
                target_mode=torch.tensor([0]), delay=torch.tensor([delay]))


class MultiSlotWholeGameModelTest(unittest.TestCase):
    def test_one_backbone_and_causal_ordered_slots(self):
        torch.manual_seed(12)
        model = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                        maximum_slots=3).eval()
        source = WholeGameModel(width=64, mixture_components=3).eval()
        model.load_state_dict(source.state_dict(), strict=False)
        sample = batch()
        first = [token(0, "train", 1), token(2, "hold", 3)]
        future_changed = [token(0, "train", 1), token(0, "train", 7)]
        prior_changed = [token(2, "hold", 2), token(2, "hold", 3)]
        output = model.forward_slots(sample, teacher_tokens=first, slots=2)
        later = model.forward_slots(sample, teacher_tokens=future_changed, slots=2)
        earlier = model.forward_slots(sample, teacher_tokens=prior_changed, slots=2)
        baseline = source(sample)
        self.assertTrue(torch.allclose(output["backbone"]["memory"],
                                       baseline["memory"], atol=1e-6))
        for name in ("stop", "actor", "kind", "target_mode", "delay"):
            self.assertTrue(torch.allclose(output["slots"][0][name],
                                            later["slots"][0][name]))
        self.assertTrue(torch.allclose(output["slots"][1]["actor"],
                                        later["slots"][1]["actor"]))
        self.assertFalse(torch.allclose(output["slots"][1]["actor"],
                                        earlier["slots"][1]["actor"]))
        self.assertTrue(torch.isneginf(output["slots"][0]["actor"][0, 1]))
        self.assertTrue(torch.isneginf(output["slots"][0]["target"][0, 1]))

    def test_slot_heads_receive_gradients(self):
        model = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                        maximum_slots=2)
        output = model.forward_slots(batch(), teacher_tokens=[token(0, "train", 4)],
                                     slots=1)["slots"][0]
        loss = (F.cross_entropy(output["stop"], torch.tensor([0])) +
                F.cross_entropy(output["actor"], torch.tensor([0])) +
                F.cross_entropy(output["kind"], torch.tensor([13])) +
                F.cross_entropy(output["delay"], torch.tensor([4])))
        loss.backward()
        for layer in (model.slot_stop, model.slot_actor_query,
                      model.slot_kind, model.slot_delay):
            self.assertGreater(layer.weight.grad.abs().sum().item(), 0)

    def test_teacher_cannot_choose_enemy_actor(self):
        model = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                        maximum_slots=1)
        with self.assertRaisesRegex(ValueError, "teacher actor is masked"):
            model.forward_slots(batch(), teacher_tokens=[token(1, "train", 0)])

    def test_previous_target_changes_later_slot(self):
        torch.manual_seed(18)
        model = MultiSlotWholeGameModel(width=64, mixture_components=3,
                                        maximum_slots=2).eval()
        near = token(0, "move", 2)
        far = token(0, "move", 2)
        near.update(position=torch.tensor([[0.1, 0.2]]),
                    has_position=torch.tensor([True]))
        far.update(position=torch.tensor([[0.8, 0.9]]),
                   has_position=torch.tensor([True]))
        continuation = token(2, "hold", 3)
        left = model.forward_slots(batch(), teacher_tokens=[near, continuation],
                                   slots=2)["slots"]
        right = model.forward_slots(batch(), teacher_tokens=[far, continuation],
                                    slots=2)["slots"]
        self.assertTrue(torch.allclose(left[0]["actor"], right[0]["actor"]))
        self.assertFalse(torch.allclose(left[1]["actor"], right[1]["actor"]))


if __name__ == "__main__":
    unittest.main()
