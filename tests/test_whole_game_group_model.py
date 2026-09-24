import unittest

import torch

from training.whole_game_group_model import GroupCommandModel, position_classes, class_position
from training.whole_game_group_batch import conditioned_action_loss
from training.whole_game_model import KINDS


def batch():
    return {"type": torch.tensor([[64, 65, 66]]), "relation": torch.tensor([[0, 1, 0]]),
            "order": torch.zeros((1, 3), dtype=torch.long),
            "entity_numeric": torch.zeros((1, 3, 16)),
            "entity_mask": torch.ones((1, 3), dtype=torch.bool),
            "spatial": torch.rand((1, 3, 8, 8)), "global": torch.zeros((1, 216))}


def token():
    return dict(actor_set=torch.tensor([[True, False, True]]), kind=torch.tensor([KINDS.index("attack_move")]),
                target_mode=torch.tensor([2]), delay=torch.tensor([3]), target_entity=torch.tensor([-1]),
                position=torch.tensor([[.23, .81]]), has_position=torch.tensor([True]), unit_type=torch.tensor([0]))


class GroupCommandTest(unittest.TestCase):
    def test_position_roundtrip_pixel_bound_and_edges(self):
        xy = torch.cat((torch.rand(1000, 2), torch.tensor([[0., 0.], [1., 1.]])))
        coarse, sub = position_classes(xy)
        decoded = class_position(coarse, sub)
        self.assertLessEqual(float((decoded - xy).abs().max()), 1 / 256 + 1e-7)
        self.assertTrue(((decoded > 0) & (decoded < 1)).all())

    def test_selected_groups_own_only_and_bounded_by_available_units(self):
        model = GroupCommandModel(width=64, maximum_slots=2).eval()
        with torch.no_grad():
            model.group_count.weight.zero_()
            model.group_count.bias.zero_()
            model.group_count.bias[-1] = 20
        output = model.forward_slots(batch())["slots"][0]
        self.assertEqual(output["chosen_actor_set"].tolist(), [[True, False, True]])
        self.assertTrue(torch.isneginf(output["target"][0, 1]))

    def test_reject_enemy_in_teacher_group(self):
        model = GroupCommandModel(width=64, maximum_slots=1).eval()
        teacher = token()
        teacher["actor_set"][0, 1] = True
        with self.assertRaisesRegex(ValueError, "actor set"):
            model.forward_slots(batch(), teacher_tokens=[teacher])

    def test_future_teacher_token_cannot_change_first_slot(self):
        model = GroupCommandModel(width=64, maximum_slots=2).eval()
        sample, first, second = batch(), token(), token()
        left = model.forward_slots(sample, teacher_tokens=[first, second])["slots"][0]
        second["position"] = torch.tensor([[.8, .1]])
        second["actor_set"] = torch.tensor([[True, False, False]])
        right = model.forward_slots(sample, teacher_tokens=[first, second])["slots"][0]
        for key in ("actor", "kind", "position_cell", "position_subcell"):
            self.assertTrue(torch.equal(left[key], right[key]), key)

    def test_supervised_position_group_and_encoder_receive_gradients(self):
        model = GroupCommandModel(width=64, maximum_slots=1)
        output = model.forward_slots(batch(), teacher_tokens=[token()])["slots"][0]
        label = dict(kind=token()["kind"], target_position=token()["position"],
                     actor_known=torch.tensor([[True, False, True]]),
                     actor_positive=torch.tensor([[True, False, True]]),
                     mask=dict(kind=True, actors=True, target_position=True))
        loss = conditioned_action_loss(output, label)
        self.assertTrue(torch.isfinite(loss))
        loss.backward()
        for layer in (model.group_count, model.position_cell, model.position_subcell,
                      model.rel_value, model.local_map):
            self.assertGreater(float(layer.weight.grad.abs().sum()), 0)


if __name__ == "__main__":
    unittest.main()
