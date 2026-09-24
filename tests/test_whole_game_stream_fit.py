import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_stream_fit import plan_chunks, verify_chunks
from training.whole_game_release import key


class StreamFitTests(unittest.TestCase):
    def test_cohort_is_exact_and_chunked_without_duplicates(self):
        with tempfile.TemporaryDirectory() as directory:
            release = Path(directory)
            selected = []
            quality = {"games": {}}
            for matchup_index, matchup in enumerate(("PvT", "PvZ", "PvP")):
                for index in range(3):
                    game_id = "game:" + f"{matchup_index * 3 + index + 1:064x}"
                    record = dict(game_id=game_id, split="train", matchup=matchup)
                    selected.append(record)
                    receipt = release / "games" / key(record) / "receipt.json"
                    receipt.parent.mkdir(parents=True)
                    receipt.write_text("{}")
                    quality["games"][game_id] = dict(
                        mmr_claim=2400, actor_group=f"{matchup}-{index}")
            (release / "identity.json").write_text(json.dumps(dict(selected=selected)))
            groups, counts = plan_chunks(
                release, quality, games_per_matchup=3, chunk_games_per_matchup=2,
                seed=42, high_mmr_threshold=2300, high_mmr_share=1.0)
            self.assertEqual(len(groups), 2)
            verify_chunks(release, groups, games_per_matchup=3,
                          chunk_games_per_matchup=2)
            for matchup in ("PvT", "PvZ", "PvP"):
                ids = [item for group in groups for item in group[matchup]]
                self.assertEqual(len(ids), 3)
                self.assertEqual(len(set(ids)), 3)
                self.assertEqual(counts[matchup]["selected"], 3)
            with self.assertRaisesRegex(ValueError, "not fully extracted"):
                plan_chunks(release, quality, games_per_matchup=4,
                            chunk_games_per_matchup=2, seed=42,
                            high_mmr_threshold=2300, high_mmr_share=1.0)
            duplicate = json.loads(json.dumps(groups))
            duplicate[1]["PvT"][0] = duplicate[0]["PvT"][0]
            with self.assertRaisesRegex(ValueError, "duplicate"):
                verify_chunks(release, duplicate, games_per_matchup=3,
                              chunk_games_per_matchup=2)


if __name__ == "__main__":
    unittest.main()
