import copy
import json
from pathlib import Path
import sys
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.replay_cohort import freeze, qualified, stable
from training.replay_pipeline import validate_launch


def record(i):
    return {"path": f"PvP/{i:04}.rep", "sha256": f"{i:064x}", "parse_status": "parsed",
            "quarantine_reasons": [], "frames": 5000, "map_sha256": "a" * 64,
            "map_name": "Backrooms", "start_time": f"2026-09-{1+i//24:02}T{i%24:02}:00:00+00:00",
            "players": [{"slot_id": 3, "name": f"p{i}", "race": "P", "source_claims": [
                {"aurora_id": i, "mmr_claim": 2200}]}, {"slot_id": 2, "name": f"enemy{i}",
                "race": "P", "source_claims": []}]}


class PipelineTests(unittest.TestCase):
    def test_quality_is_per_player_and_pro_claims_do_not_override_mmr(self):
        item = record(0)
        self.assertTrue(qualified(item["players"][0]))
        self.assertFalse(qualified(item["players"][1]))
        item["players"][1]["source_claims"] = [{"pro_id_claim": 123, "mmr_claim": None}]
        self.assertFalse(qualified(item["players"][1]))

    def test_duplicate_prefix_recording_and_both_perspectives_stay_together(self):
        records = [record(i) for i in range(60)]
        duplicate = copy.deepcopy(records[0])
        duplicate.update(path="PvP/duplicate.rep", sha256="e" * 64, frames=8000)
        cohort = freeze(records + [duplicate])
        self.assertEqual(len(cohort["games"]), 60)
        self.assertEqual(len(cohort["duplicate_aliases"]), 1)
        for game in cohort["games"]:
            self.assertEqual(game["slots"], [2, 3])
            self.assertEqual(game["player_quality"], ["unknown", "qualified_ladder"])
            if game["split"] == "train":
                self.assertLess(game["timestamp"], cohort["train_before"])
                self.assertFalse(game["player_holdout"])
                self.assertFalse(game["map_holdout"])

    def test_map_and_player_holdouts_never_train(self):
        records = [record(i) for i in range(60)]
        records[0]["map_name"] = "\x07R\x06a\x07d\x06e\x07o\x06n 1.2"
        cohort = freeze(records)
        self.assertEqual(next(g for g in cohort["games"] if g["path"] == records[0]["path"])["split"], "test")
        for game in cohort["games"]:
            if any(int(key[:8], 16) % 100 < 5 for key in game["player_keys"]):
                self.assertEqual(game["split"], "test")

    def test_cuda_gate_refuses_incomplete_validation_mass_failure_or_empty_splits(self):
        games = [{"game_id": str(i), "split": ("train", "validation", "test")[i%3],
                  "player_keys": [], "map_holdout": False} for i in range(300)]
        results = {g["game_id"]: {"status": "extracted"} for g in games}
        summary = {"complete": True, "processed": 30019, "total": 30019, "matched": 30019}
        self.assertEqual(len(validate_launch(summary, games, results)), 300)
        for changes in ({"complete": False}, {"processed": 100}, {"matched": 20000}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                validate_launch({**summary, **changes}, games, results)
        with self.assertRaises(ValueError):
            validate_launch(summary, games, {})
        games[0]["map_holdout"] = True
        with self.assertRaisesRegex(ValueError, "held-out"):
            validate_launch(summary, games, results)


if __name__ == "__main__":
    unittest.main()
