import json
from contextlib import closing
from pathlib import Path
import sqlite3
import tempfile
import unittest

from training.whole_game_quality import build_index, load_index


class WholeGameQualityTests(unittest.TestCase):
    def test_claim_is_bound_to_exact_protoss_perspective_and_replay_hash(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            identity = root / "identity.json"
            audit = root / "audit.sqlite"
            index_path = root / "quality.json"
            row = dict(game_id="g1", split="train", matchup="PvT", path="x.rep",
                       replay_sha256="a" * 64, perspective=1)
            identity.write_text(json.dumps(dict(selected=[row])))
            players = [dict(slot_id=0, race="T", name="opponent", source_claims=[
                dict(catalog_side="primary", mmr_claim=2700)]),
                dict(slot_id=2, race="P", name="protoss", source_claims=[
                    dict(catalog_side="opponent", mmr_claim=2311)])]
            with closing(sqlite3.connect(audit)) as connection:
                connection.execute("CREATE TABLE replays (path TEXT,sha256 TEXT,status TEXT,detail TEXT)")
                connection.execute("INSERT INTO replays VALUES(?,?,?,?)",
                                   ("x.rep", "a" * 64, "parsed", json.dumps(dict(players=players))))
                connection.commit()
            result = build_index(identity, audit)
            self.assertEqual(result["games"]["g1"]["mmr_claim"], 2311)
            self.assertEqual(len(result["games"]["g1"]["actor_group"]), 64)
            index_path.write_text(json.dumps(result))
            self.assertEqual(load_index(index_path, identity)["counts"]["train"]["PvT"]["mmr_ge_2300"], 1)
            row["split"] = "test"
            identity.write_text(json.dumps(dict(selected=[row])))
            with self.assertRaises(ValueError):
                build_index(identity, audit)
            with self.assertRaises(ValueError):
                load_index(index_path, identity)


if __name__ == "__main__":
    unittest.main()
