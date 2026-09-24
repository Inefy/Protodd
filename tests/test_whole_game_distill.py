import unittest
import tempfile
from pathlib import Path
import json
from types import SimpleNamespace
from unittest.mock import patch

import torch

from tests.test_whole_game_batch import action_label, row
from training.whole_game_batch import minibatch_loss
from training.whole_game_conditional_model import ConditionalWholeGameModel
from training.whole_game_distill import (cadence_action_buckets, checkpoint_model,
                                        distillation_loss, paired_forward,
                                        supervised_output_loss)
from training.whole_game_fit import SCHEMA as FIT_SCHEMA
from training.whole_game_features import static_grid
from training.whole_game_model import WholeGameModel
from training.whole_game_release import key


class WholeGameDistillTests(unittest.TestCase):
    def test_reused_student_forward_matches_supervision_and_gradients(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        device = torch.device("cpu")
        for task in ("action", "event", "forecast"):
            samples = []
            for count in (1, 3):
                supervision = (dict(event=None, action=action_label(), update_memory=False)
                               if task == "action" else
                               dict(event=count % 2, action=None, update_memory=True)
                               if task == "event" else
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
            model = WholeGameModel(width=64, mixture_components=3).eval()
            _, output, _, ids = paired_forward(model, model, samples, device)
            reused = supervised_output_loss(output, samples, ids, device)
            original = minibatch_loss(model, samples, device)
            self.assertTrue(torch.allclose(reused, original, atol=1e-6, rtol=1e-6))
            original.backward()
            expected = {name: parameter.grad.clone() if parameter.grad is not None else None
                        for name, parameter in model.named_parameters()}
            model.zero_grad(set_to_none=True)
            reused.backward()
            for name, parameter in model.named_parameters():
                if expected[name] is None:
                    self.assertIsNone(parameter.grad, (task, name))
                else:
                    self.assertTrue(torch.allclose(parameter.grad, expected[name],
                                                   atol=1e-6, rtol=1e-5), (task, name))

    def test_cadence_distillation_uses_complete_training_cohort(self):
        with tempfile.TemporaryDirectory() as directory:
            release = Path(directory)
            records = []
            quality = {"games": {}}
            for index, matchup in enumerate(("PvT", "PvZ", "PvP")):
                record = dict(game_id="game:" + str(index + 1) * 64,
                              split="train", matchup=matchup)
                records.append(record)
                quality["games"][record["game_id"]] = dict(
                    mmr_claim=2400, actor_group=f"player-{index}")
                shard = release / "games" / key(record)
                shard.mkdir(parents=True)
                (shard / "receipt.json").write_text("{}")
            (release / "identity.json").write_text(json.dumps({"selected": records}))
            args = SimpleNamespace(games_per_matchup=1, seed=42,
                                   high_mmr_threshold=2300, high_mmr_share=0.5,
                                   history=8, bucket_limit=64, per_game_bucket_limit=2)
            with patch("training.whole_game_distill.collect_cadence_actions",
                       return_value=({"action": []}, {"games": 3})) as collector:
                buckets, info = cadence_action_buckets(release, quality, args)
            self.assertEqual(info["games"], 3)
            self.assertEqual(buckets, {"action": []})
            self.assertEqual({k: len(v) for k, v in collector.call_args.args[1].items()},
                             {"PvT": 1, "PvZ": 1, "PvP": 1})

    def test_conditional_teacher_checkpoint_loads_strictly(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "teacher.pt"
            teacher = ConditionalWholeGameModel(width=128, mixture_components=3)
            torch.save(dict(schema=FIT_SCHEMA, state_dict=teacher.state_dict(),
                            source_identity_sha256="release"), path)
            checkpoint, loaded = checkpoint_model(path, torch.device("cpu"))
            self.assertIsInstance(loaded, ConditionalWholeGameModel)
            self.assertEqual(checkpoint["source_identity_sha256"], "release")

    def test_teacher_soft_targets_preserve_padding_and_student_gradients(self):
        terrain = static_grid(dict(schema="protodd-terrain-v2", width_walktiles=8,
                                   height_walktiles=8, walkability="1" * 64))
        samples = [([row("cadence", count)], row("before_command", count),
                    dict(event=None, action=action_label(), update_memory=False), terrain, {})
                   for count in (1, 3)]
        teacher = WholeGameModel(width=128, mixture_components=3).eval()
        student = WholeGameModel(width=64, mixture_components=3).train()
        teacher_output, student_output, batch, ids = paired_forward(
            teacher, student, samples, torch.device("cpu"))
        self.assertEqual(batch["entity_mask"].sum(dim=1).tolist(), [1, 3])
        loss = distillation_loss(teacher_output, student_output, batch, ids, samples)
        self.assertTrue(torch.isfinite(loss))
        loss.backward()
        self.assertGreater(student.entity[0].weight.grad.abs().sum().item(), 0)
        self.assertIsNone(teacher.entity[0].weight.grad)


if __name__ == "__main__":
    unittest.main()
