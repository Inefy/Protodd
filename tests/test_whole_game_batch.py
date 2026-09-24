import unittest

import torch

from training.whole_game_batch import minibatch_loss
from training.whole_game_encoding_cache import ObservationCache
from training.whole_game_features import static_grid
from training.whole_game_fit import sample_loss
from training.whole_game_model import WholeGameModel
from training.whole_game_pilot import SCHEMA


def row(reason, count):
    own_state = dict(energy=0, ground_cooldown=0, air_cooldown=0, order=6,
                     order_position=[10, 10], loaded=0, queue=[], cargo=[], order_target=-1)
    entities = [dict(id=n, type=64, relation=0, position=[12 + n, 12],
                     hp=20, shields=20, visible=1, completed=1,
                     first_seen=0, last_seen=24, own_state=own_state) for n in range(count)]
    return dict(schema=SCHEMA, reason=reason, frame=24, vision="2211", entities=entities,
                minerals=50, gas=0, supply_used=8, supply_total=18,
                technology_completed=[0] * 44, technology_in_progress=[0] * 44,
                upgrade_levels=[0] * 61, upgrade_in_progress=[0] * 61)


def action_label():
    return dict(schema="protodd-imitation-label-v1", domain="unit_control",
                coordinate_space="pixel", actor_positive=[0], actor_negative=[],
                actor_unknown=[], loss_masks=dict(kind=True, actors=True, queued=True,
                    target_mode=True, target_entity=False, target_position=True,
                    order=False, unit_type=False, technology=False, upgrade=False,
                    queue_slot=False), actions=dict(kind="move", queued=False,
                    target_mode="position", target_position=[20, 20]))


class WholeGameBatchTests(unittest.TestCase):
    def test_batched_event_and_action_loss_match_separate_forwards_with_padding(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        model = WholeGameModel(width=64, mixture_components=3).eval()
        device = torch.device("cpu")
        for task in ("event", "action", "forecast"):
            samples = []
            for count in (1, 3):
                supervision = (dict(event=count % 2, action=None, update_memory=True)
                               if task == "event" else
                               dict(event=None, action=action_label(), update_memory=False)
                               if task == "action" else
                               dict(forecast=dict(newly_explored_tiles=count,
                                    newly_known_enemy_ids_retained=0,
                                    own_hp_shield_loss_common=0,
                                    technology_completed_gain=0, upgrade_level_gain=0,
                                    visible_enemies_at_target=0,
                                    confirmed_command_domains=[0] * 5), horizon=24,
                                    update_memory=True))
                samples.append(([row("cadence", count)],
                                row("before_command" if task == "action" else "cadence", count),
                                supervision, terrain, {}))
            independent = torch.stack([sample_loss(model, sample, device)[0]
                                       for sample in samples]).mean()
            batched = minibatch_loss(model, samples, device)
            self.assertTrue(torch.allclose(independent, batched, atol=1e-5, rtol=1e-5))
            cache = ObservationCache(1_000_000)
            self.assertTrue(torch.allclose(batched, minibatch_loss(model, samples, device, cache)))
            self.assertTrue(torch.allclose(batched, minibatch_loss(model, samples, device, cache)))
            self.assertGreater(cache.hits, 0)
            model.zero_grad(set_to_none=True)
            batched.backward()
            self.assertGreater(model.entity[0].weight.grad.abs().sum().item(), 0)


if __name__ == "__main__":
    unittest.main()
