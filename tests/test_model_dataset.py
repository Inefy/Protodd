"""Standard-library data gates run even on machines without PyTorch."""
import copy
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.prepare import prepare, validate_manifest, validate_sample
from training.schema import load_schema

TOOL = None
SCHEMA = load_schema()


def fixture():
    games = []
    rows = []
    for i, split in enumerate(("train", "validation", "test")):
        gid = f"game-{i}"
        games.append(dict(game_id=gid, duplicate_group=gid, split=split,
                          replay_sha256=hashlib.sha256(gid.encode()).hexdigest(),
                          races=["P", "P"], player_quality=["verified_pro", "qualified_ladder"],
                          valid_through_frame=24000))
        for j in range(24):
            values = [0.0] * len(SCHEMA["features"])
            values[0] = j * 24 / 86400
            # Synthetic separable classification fixture, not replay evidence.
            values[1] = float(j % 2)
            rows.append(dict(game_id=gid, perspective=j % 2, frame=j * 24,
                             action_frame=j * 24, features=values,
                             allowed_actions=["wait", "train_probe", "build_pylon"],
                             action="train_probe" if j % 2 else "build_pylon", confidence=1.0))
    manifest = dict(schema=SCHEMA["version"], fingerprint=SCHEMA["fingerprint"],
                    source="synthetic_test", extractor="unit-test-v1", audit_id="test-fixture",
                    validation=dict(playback=True, perspective=True, actions=True), games=games)
    return manifest, rows


def write_fixture(root):
    manifest, rows = fixture()
    m, s = Path(root) / "manifest.json", Path(root) / "samples.jsonl"
    m.write_text(json.dumps(manifest), encoding="utf-8")
    s.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")
    return m, s


class DataTests(unittest.TestCase):
    def test_compiled_schema_matches(self):
        if TOOL is None:
            self.skipTest("pass model_tool to verify C++ schema")
        actual = json.loads(subprocess.check_output([str(TOOL), "schema"], text=True))
        self.assertEqual(SCHEMA, actual)

    def test_prepare_and_no_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            m, s = write_fixture(directory)
            out = Path(directory) / "dataset.sqlite"
            result = prepare(m, s, out, True)
            self.assertEqual(result["counts"], dict(train=24, validation=24, test=24))
            with closing(sqlite3.connect(out)) as db:
                self.assertEqual(db.execute("SELECT COUNT(*) FROM samples").fetchone()[0], 72)
            with self.assertRaisesRegex(ValueError, "exists"):
                prepare(m, s, out, True)

    def test_synthetic_requires_explicit_flag_and_evaluation_rejected(self):
        manifest, _ = fixture()
        with self.assertRaises(ValueError):
            validate_manifest(manifest, SCHEMA)
        manifest["source"] = "bot_evaluation"
        with self.assertRaises(ValueError):
            validate_manifest(manifest, SCHEMA, True)

    def test_duplicate_game_hash_and_group_cannot_leak(self):
        for field in ("game_id", "replay_sha256", "duplicate_group"):
            manifest, _ = fixture()
            manifest["games"][1][field] = manifest["games"][0][field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                validate_manifest(manifest, SCHEMA, True)

    def test_bad_labels_and_features(self):
        manifest, rows = fixture()
        games = validate_manifest(manifest, SCHEMA, True)
        changes = [dict(action="train_carrier"), dict(allowed_actions=["train_probe"]),
                   dict(perspective=2), dict(frame=True), dict(confidence=float("nan")),
                   dict(action_frame=999999), dict(enemy_bank=1000)]
        for change in changes:
            row = copy.deepcopy(rows[0]); row.update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                validate_sample(row, games, SCHEMA)
        for bad in (float("nan"), float("inf"), -1, 17, True):
            row = copy.deepcopy(rows[0]); row["features"][1] = bad
            with self.subTest(value=bad), self.assertRaises(ValueError):
                validate_sample(row, games, SCHEMA)

    def test_quality_is_per_acting_player(self):
        manifest, rows = fixture()
        manifest["games"][0]["player_quality"][1] = "unknown"
        games = validate_manifest(manifest, SCHEMA, True)
        validate_sample(rows[0], games, SCHEMA)
        with self.assertRaisesRegex(ValueError, "quality"):
            validate_sample(rows[1], games, SCHEMA)

    def test_future_feature_frame_and_failed_prepare(self):
        manifest, rows = fixture()
        games = validate_manifest(manifest, SCHEMA, True)
        rows[0]["features"][0] = 0.5
        with self.assertRaisesRegex(ValueError, "feature frame"):
            validate_sample(rows[0], games, SCHEMA)
        with tempfile.TemporaryDirectory() as directory:
            m, s = write_fixture(directory)
            with s.open("a", encoding="utf-8") as dest:
                dest.write(json.dumps(fixture()[1][0]) + "\n")
            out = Path(directory) / "dataset.sqlite"
            with self.assertRaisesRegex(ValueError, "sample line"):
                prepare(m, s, out, True)
            self.assertFalse(out.exists())
            self.assertFalse(out.with_name(out.name + ".partial").exists())


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        TOOL = Path(sys.argv.pop(1)).resolve()
    unittest.main()
