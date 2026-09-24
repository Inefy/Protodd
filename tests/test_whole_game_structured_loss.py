import unittest

import torch

from training.whole_game_action_schema import select_supported_pair
from training.whole_game_features import static_grid
from training.whole_game_model import KINDS, TARGET_MODES, WholeGameModel, masked_action_loss
from training.whole_game_structured_batch import structured_minibatch_loss
from training.whole_game_structured_loss import (balanced_actor_loss, structured_action_loss,
                                                 target_compatibility_loss)
from tests.test_whole_game_batch import action_label, row


class StructuredActionLossTests(unittest.TestCase):
    def test_one_actor_keeps_gradient_with_large_army(self):
        def gradients(count, balanced):
            logits = torch.zeros((1, count), requires_grad=True)
            known = torch.ones_like(logits, dtype=torch.bool)
            positive = torch.zeros_like(logits, dtype=torch.bool)
            positive[0, 0] = True
            if balanced:
                loss = balanced_actor_loss(logits, known, positive)
            else:
                loss = torch.nn.functional.binary_cross_entropy_with_logits(
                    logits[known], positive[known].float())
            loss.backward()
            return logits.grad[0]

        small = gradients(4, True)
        army = gradients(128, True)
        baseline = gradients(128, False)
        self.assertAlmostEqual(small[0].item(), -0.5, places=6)
        self.assertAlmostEqual(army[0].item(), -0.5, places=6)
        self.assertAlmostEqual(army[1].item(), 0.5 / 127, places=6)
        self.assertAlmostEqual(baseline[0].item(), -0.5 / 128, places=6)

    def test_replaces_only_actor_term_and_respects_unknown_units(self):
        kind = torch.zeros((1, len(KINDS)), requires_grad=True)
        mode = torch.zeros((1, len(TARGET_MODES)), requires_grad=True)
        actor = torch.zeros((1, 4), requires_grad=True)
        label = dict(mask=dict(kind=True, target_mode=True, actors=True),
                     kind=torch.tensor([KINDS.index("attack")]),
                     target_mode=torch.tensor([TARGET_MODES.index("position")]),
                     actor_known=torch.tensor([[True, True, False, True]]),
                     actor_positive=torch.tensor([[True, False, False, False]]))
        output = dict(kind=kind, target_mode=mode, actor=actor)
        original, old_parts = masked_action_loss(output, label)
        structured, new_parts = structured_action_loss(output, label)
        expected = (old_parts["kind"] + old_parts["target_mode"] + balanced_actor_loss(
            actor, label["actor_known"], label["actor_positive"]) +
            target_compatibility_loss(kind, mode))
        self.assertTrue(torch.allclose(structured.detach(), expected))
        self.assertTrue(torch.allclose(new_parts["kind"], old_parts["kind"]))
        self.assertGreater(structured.item(), original.item())
        structured.backward()
        self.assertAlmostEqual(actor.grad[0, 0].item(), -0.5, places=6)
        self.assertEqual(actor.grad[0, 2].item(), 0.0)
        self.assertGreater(kind.grad.abs().sum().item(), 0)

    def test_joint_projection_and_compatibility_gradient(self):
        kind = torch.full((1, len(KINDS)), -20.0, requires_grad=True)
        mode = torch.tensor([[4.0, -2.0, 3.0]], requires_grad=True)
        with torch.no_grad():
            kind[0, KINDS.index("build")] = 5.0
            kind[0, KINDS.index("train")] = 4.0
        self.assertEqual(select_supported_pair(kind[0], mode[0]), ("build", "position"))
        penalty = target_compatibility_loss(kind, mode)
        self.assertGreater(penalty.item(), 0)
        penalty.backward()
        self.assertGreater(kind.grad[0, KINDS.index("build")].abs().item(), 0)
        self.assertGreater(mode.grad[0, TARGET_MODES.index("none")].abs().item(), 0)

    def test_missing_confirmed_actor_is_rejected(self):
        logits = torch.zeros((1, 3))
        known = torch.ones((1, 3), dtype=torch.bool)
        positive = torch.zeros((1, 3), dtype=torch.bool)
        with self.assertRaisesRegex(ValueError, "confirmed actor"):
            balanced_actor_loss(logits, known, positive)

    def test_structured_minibatch_backpropagates_on_padded_entities(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        samples = [([row("cadence", count)], row("before_command", count),
                    dict(event=None, action=action_label(), update_memory=False), terrain, {})
                   for count in (1, 3)]
        model = WholeGameModel(width=64, mixture_components=3)
        loss = structured_minibatch_loss(model, samples, torch.device("cpu"))
        self.assertTrue(torch.isfinite(loss))
        loss.backward()
        self.assertGreater(model.actor_key.weight.grad.abs().sum().item(), 0)
        self.assertGreater(model.kind.weight.grad.abs().sum().item(), 0)


if __name__ == "__main__":
    unittest.main()
