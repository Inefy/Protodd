import unittest

import torch

from training.whole_game_features import encode_label, encode_observation, static_grid
from training.whole_game_model import WholeGameModel, masked_action_loss
from training.whole_game_pilot import SCHEMA


class WholeGameModelTests(unittest.TestCase):
    def test_real_contract_has_masked_actor_target_and_gradients(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        own_state = dict(energy=0, ground_cooldown=0, air_cooldown=0, order=6,
                         order_position=[10, 10], loaded=0, queue=[], cargo=[], order_target=-1)
        base = dict(type=64, position=[12, 12], hp=20, shields=20, visible=1,
                    completed=1, first_seen=0, last_seen=24)
        row = dict(schema=SCHEMA, reason="before_command", frame=24, vision="2211",
                   minerals=50, gas=0, supply_used=8, supply_total=18,
                   technology_completed=[0] * 44, technology_in_progress=[0] * 44,
                   upgrade_levels=[0] * 61, upgrade_in_progress=[0] * 61,
                   entities=[dict(base, id=0, relation=0, own_state=own_state),
                             dict(base, id=1, relation=1, position=[20, 20], own_state=None),
                             dict(base, id=2, relation=0, position=[25, 25], own_state=own_state)])
        batch, ids, overflow = encode_observation(row, terrain)
        self.assertEqual(overflow, 0)
        label = dict(schema="protodd-imitation-label-v1", domain="unit_control",
                     coordinate_space="pixel", actor_positive=[0], actor_negative=[],
                     actor_unknown=[2], loss_masks=dict(kind=True, actors=True, queued=True,
                        target_mode=True, target_entity=True, target_position=False,
                        order=False, unit_type=False, technology=False, upgrade=False, queue_slot=False),
                     actions=dict(kind="attack", queued=False, target_mode="entity", target_entity=1))
        target = encode_label(label, ids, 2, 2)
        self.assertFalse(target["actor_known"][0, ids[2]])
        model = WholeGameModel(width=64, mixture_components=3)
        output = model(batch)
        loss, pieces = masked_action_loss(output, target)
        self.assertTrue(torch.isfinite(loss))
        self.assertIn("actors", pieces)
        self.assertIn("target_entity", pieces)
        loss.backward()
        self.assertIsNotNone(model.entity[0].weight.grad)
        self.assertGreater(model.entity[0].weight.grad.abs().sum().item(), 0)
        self.assertEqual(output["memory"].shape, (1, 64))
        self.assertEqual(output["forecast_24"].shape, (1, 11))
        self.assertEqual(output["forecast_240"].shape, (1, 11))
        model.zero_grad(set_to_none=True)
        model(batch)["forecast_240"].square().mean().backward()
        self.assertGreater(model.forecast["240"].weight.grad.abs().sum().item(), 0)
        hidden_target = dict(label, actions=dict(label["actions"], target_entity=999))
        masked = encode_label(hidden_target, ids, 2, 2)
        self.assertFalse(masked["mask"]["target_entity"])


if __name__ == "__main__":
    unittest.main()
