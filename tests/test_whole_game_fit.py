import unittest
import json
from pathlib import Path
import tempfile
import torch

from training.whole_game_fit import (choose_games, merge_quality_buckets,
                                     summarize_validation, validate_release_pair,
                                     initialize_model, SCHEMA)
from training.whole_game_model import WholeGameModel


class WholeGameFitTests(unittest.TestCase):
    def test_initialization_requires_same_release_and_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "teacher.pt"
            teacher = WholeGameModel(width=64)
            torch.save(dict(schema=SCHEMA, source_identity_sha256="cohort-a",
                            state_dict=teacher.state_dict()), path)
            student = WholeGameModel(width=64)
            self.assertEqual(len(initialize_model(student, path, "cohort-a")), 64)
            self.assertTrue(torch.equal(student.memory.weight_hh, teacher.memory.weight_hh))
            with self.assertRaises(ValueError):
                initialize_model(student, path, "cohort-b")
            with self.assertRaises(RuntimeError):
                initialize_model(WholeGameModel(width=128), path, "cohort-a")

    def test_separate_release_validation_rejects_same_game_or_replay_file(self):
        with tempfile.TemporaryDirectory() as directory:
            train, heldout = Path(directory) / "train", Path(directory) / "heldout"
            train.mkdir(); heldout.mkdir()
            (train / "identity.json").write_text(json.dumps(dict(selected=[
                dict(split="train", game_id="game-a", replay_sha256="a")])))
            (heldout / "identity.json").write_text(json.dumps(dict(selected=[
                dict(split="validation", game_id="game-b", replay_sha256="b")])))
            hashes = validate_release_pair(train, heldout)
            self.assertEqual(len(hashes), 2)
            (heldout / "identity.json").write_text(json.dumps(dict(selected=[
                dict(split="validation", game_id="game-a", replay_sha256="b")])))
            with self.assertRaises(ValueError):
                validate_release_pair(train, heldout)
            (heldout / "identity.json").write_text(json.dumps(dict(selected=[
                dict(split="validation", game_id="game-b", replay_sha256="a")])))
            with self.assertRaises(ValueError):
                validate_release_pair(train, heldout)

    def test_quality_sampling_only_changes_training_selection(self):
        eligible = {matchup: [dict(game_id=f"{matchup}-{n}") for n in range(6)]
                    for matchup in ("PvT", "PvZ", "PvP")}
        quality = dict(games={row["game_id"]: dict(mmr_claim=2300 if row["game_id"].endswith("-5") else 2000,
                                                   actor_group=row["game_id"])
                              for rows in eligible.values() for row in rows})
        train, counts = choose_games(eligible, "train", 2, 42, quality, 2300, 0.5)
        self.assertEqual(len(train), 6)
        self.assertTrue(all(f"{matchup}-5" in train for matchup in eligible))
        self.assertTrue(all(counts[matchup]["mmr_ge_threshold"] == 1 for matchup in eligible))
        normal, _ = choose_games(eligible, "validation", 2, 42)
        held_out, _ = choose_games(eligible, "validation", 2, 42, quality, 2300, 1.0)
        self.assertEqual(normal, held_out)
        for rows in eligible.values():
            for row in rows:
                if row["game_id"].endswith(("-3", "-4")):
                    quality["games"][row["game_id"]]["mmr_claim"] = 2300
        _, exact = choose_games(eligible, "train", 4, 42, quality, 2300, 0.5)
        self.assertTrue(all(exact[matchup]["mmr_ge_threshold"] == 2 for matchup in eligible))

    def test_repeated_player_does_not_fill_quality_quota_first(self):
        eligible = {m: [dict(game_id=f"{m}-{n}") for n in range(6)]
                    for m in ("PvT", "PvZ", "PvP")}
        quality = dict(games={row["game_id"]: dict(mmr_claim=2300,
                                                   actor_group="repeat" if row["game_id"].endswith(("-0", "-1", "-2"))
                                                   else row["game_id"])
                              for rows in eligible.values() for row in rows})
        _, counts = choose_games(eligible, "train", 4, 42, quality, 2300, 1.0)
        self.assertTrue(all(counts[m]["distinct_actor_groups"] == 4 for m in eligible))

    def test_quality_reservoir_preserves_tier_mix_and_backfills_rare_actions(self):
        categories = {
            (("PvT", "unit_control", "move"), "high"): ["h1", "h2", "h3"],
            (("PvT", "unit_control", "move"), "base"): ["b1", "b2", "b3"],
            (("PvZ", "ability", "cast"), "high"): ["spell1", "spell2"],
        }
        result = merge_quality_buckets(categories, 4, 0.5)
        self.assertEqual(result[("PvT", "unit_control", "move")], ["h1", "h2", "b1", "b2"])
        self.assertEqual(result[("PvZ", "ability", "cast")], ["spell1", "spell2"])

    def test_balanced_metrics_are_grouped_by_matchup_and_domain(self):
        rows = [
            dict(category="PvT/event/1", task="event", loss=0.2, overflow=0,
                 positive=1, predicted=0.8),
            dict(category="PvT/event/0", task="event", loss=0.4, overflow=0,
                 positive=0, predicted=0.3),
            dict(category="PvT/ability/cast", task="action", domain="ability",
                 loss=2.0, overflow=1, domain_correct=1, kind_correct=0),
            dict(category="PvZ/ability/cast", task="action", domain="ability",
                 loss=3.0, overflow=0, domain_correct=0, kind_correct=0),
            dict(category="PvT/forecast/240/1000", task="forecast", horizon=240,
                 loss=0.8, overflow=0, binary_truth=[1] + [0] * 9,
                 binary_correct=[1] * 10, exploration_magnitude_error=0.1),
        ]
        result = summarize_validation(rows)
        self.assertEqual(result["PvT/event"]["samples"], 2)
        self.assertEqual(result["PvT/event"]["accuracy"], 1)
        self.assertAlmostEqual(result["PvT/event"]["mean_loss"], 0.3)
        self.assertEqual(result["PvT/ability"]["domain_accuracy"], 1)
        self.assertEqual(result["PvT/ability"]["overflow_samples"], 1)
        self.assertEqual(result["PvZ/ability"]["domain_accuracy"], 0)
        self.assertEqual(result["PvT/forecast_240"]["binary_accuracy"]["exploration"], 1)
        self.assertEqual(result["PvT/forecast_240"]["positive_rate"]["new_enemy"], 0)


if __name__ == "__main__":
    unittest.main()
