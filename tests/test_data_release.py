from contextlib import closing
import json
from pathlib import Path
import sqlite3
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from training import data_release as release
from training import replay_pipeline
from training.schema import load_schema


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


class ReleaseTests(unittest.TestCase):
    def test_publication_never_overwrites_an_existing_release(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "release.json"
            release.immutable_json(path, {"complete": True})
            release.immutable_json(path, {"complete": True})
            with self.assertRaisesRegex(ValueError, "Immutable"):
                release.immutable_json(path, {"complete": False})
            self.assertEqual(release.read_json(path), {"complete": True})

    def test_legacy_entry_point_cannot_launch_a_training_subprocess(self):
        with patch.object(replay_pipeline.subprocess, "run") as process:
            with self.assertRaisesRegex(SystemExit, "retired"):
                replay_pipeline.main()
            process.assert_not_called()

    def test_only_cli_change_and_unrelated_trainer_change_can_reuse_extraction(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            snapshot = root / "archive"
            hashes, archived = {}, {}
            for name in release.EXTRACTION_SOURCES | {"train.py"}:
                source = root / "training" / name
                source.parent.mkdir(exist_ok=True)
                source.write_text("def extract():\n    return 1\ndef main():\n    return 2\n")
                target = snapshot / "training" / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(source.read_bytes())
                hashes[str(source)] = release.digest(source)
                archived[name] = release.digest(source)
            write(snapshot / "snapshot.json", {"files": archived})
            identity = {"config": {"python": "trainer-python.exe", "model_tool": "model-tool.exe"}, "hashes": hashes}
            pipeline = root / "training/replay_pipeline.py"
            pipeline.write_text(pipeline.read_text().replace("return 2", "raise SystemExit('retired')"))
            (root / "training/train.py").write_text("# unrelated trainer has evolved\n")
            pins = release.verify_pins(identity, snapshot, root)
            self.assertIn(str(root / "training/train.py"), pins["historical_training_hashes"])
            pipeline.write_text(pipeline.read_text().replace("return 1", "return 3"))
            with self.assertRaisesRegex(ValueError, "Pinned extraction input"):
                release.verify_pins(identity, snapshot, root)
            (snapshot / "training/schema.py").write_text("tampered")
            with self.assertRaises(ValueError):
                release.verify_pins(identity, snapshot, root)

    def test_cache_requires_both_payload_hashes_and_matching_checkpoint(self):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            game = {"game_id": "g", "replay_sha256": "f" * 64, "valid_through_frame": 2400}
            schema = load_schema()
            (directory / "samples.jsonl.gz").write_bytes(b"samples")
            (directory / "actions.jsonl.gz").write_bytes(b"actions")
            record = {"status": "extracted", "game_id": "g", "samples": 50, "counts": {"wait": 40, "train_probe": 10},
                      "checkpoints_sha256": "c" * 64, "stats": {"schema": schema["version"],
                      "fingerprint": schema["fingerprint"], "end_frame": 2400},
                      "samples_sha256": release.digest(directory / "samples.jsonl.gz"),
                      "actions_sha256": release.digest(directory / "actions.jsonl.gz")}
            validation = {"status": "checkpoints_matched", "sha256": game["replay_sha256"],
                          "native_checkpoints_sha256": "c" * 64}
            release.verify_cached(game, record, directory, validation)
            with self.assertRaisesRegex(ValueError, "playback evidence"):
                release.verify_cached(game, record, directory, {**validation, "native_checkpoints_sha256": "d" * 64})
            (directory / "actions.jsonl.gz").write_bytes(b"changed actions")
            with self.assertRaisesRegex(ValueError, "actions.jsonl.gz"):
                release.verify_cached(game, record, directory, validation)

    def test_complete_summary_cannot_hide_missing_or_mismatched_rows(self):
        expected = {"one.rep": "a"}
        records = {"one.rep": {"status": "checkpoints_matched", "audit_sha256": "a"}}
        summary = {"complete": True, "total": 1, "processed": 1, "pending": 0, "matched": 1, "quarantined": 0}
        release.final_validation(summary, records, expected)
        for bad in ({**summary, "complete": False}, {**summary, "matched": 0}):
            with self.assertRaises(ValueError):
                release.final_validation(bad, records, expected)
        with self.assertRaises(ValueError):
            release.final_validation(summary, {}, expected)
        with self.assertRaises(ValueError):
            release.final_validation(summary, records, {"one.rep": "b"})

    def test_lock_blocks_second_worker_then_releases(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp) / "lock"
            with release.release_lock(path):
                with self.assertRaisesRegex(ValueError, "owns this output"):
                    with release.release_lock(path):
                        self.fail("second worker acquired lock")
            with release.release_lock(path):
                pass

    def test_different_destinations_cannot_write_same_legacy_games(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            old = base / "old"
            old.mkdir()
            config = {name: str(base / name) for name in (
                "root", "audit", "validation", "extractor", "decoder", "mpq", "assets", "evidence")}
            config["output"] = str(old)
            config_path = base / "config.json"
            write(config_path, config)
            with release.release_lock(old / "extraction-release.lock"), patch.object(release, "_run") as work:
                with self.assertRaisesRegex(ValueError, "owns this output"):
                    release.run(config_path, base / "other-release", base / "snapshot")
                work.assert_not_called()

    def test_completed_extraction_publishes_release_without_training(self):
        # Small payloads but a real 300-game manifest exercise the actual 100/split
        # launch gate, cached integrity checks and atomic publication path.
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            old, output, snapshot = base / "old", base / "release", base / "snapshot"
            old.mkdir(); output.mkdir(); snapshot.mkdir()
            schema = load_schema()
            config = SimpleNamespace(output=old, validation=base / "validation", audit=base / "audit.sqlite",
                                     evidence=base / "evidence.json", extractor=base / "extractor.exe", assets=base / "assets")
            config.extractor.write_bytes(b"pinned extractor")
            write(config.evidence, {"passed": True, "extractor_sha256": release.digest(config.extractor),
                                   "schema": schema["version"], "fingerprint": schema["fingerprint"]})
            write(old / "identity.json", {"config": {}, "schema": schema, "terrain": {}, "hashes": {}})
            write(snapshot / "snapshot.json", {"legacy_identity_sha256": release.digest(old / "identity.json")})
            write(config.validation / "identity.json", {"hashes": {}})
            games, records = [], []
            for i in range(300):
                game = {"game_id": f"g{i}", "path": f"{i}.rep", "replay_sha256": f"{i:064x}",
                        "duplicate_group": f"d{i}", "split": ("train", "validation", "test")[i % 3],
                        "races": ["P", "T"], "player_quality": ["qualified_ladder", "unknown"],
                        "valid_through_frame": 2400, "player_keys": [], "map_holdout": False}
                games.append(game)
                directory = old / "games" / game["replay_sha256"]
                directory.mkdir(parents=True)
                (directory / "samples.jsonl.gz").write_bytes(b"preserved samples")
                (directory / "actions.jsonl.gz").write_bytes(b"preserved actions")
                record = {"status": "extracted", "game_id": game["game_id"], "samples": 50,
                          "counts": {"wait": 40, "train_probe": 10}, "checkpoints_sha256": "c" * 64,
                          "stats": {"schema": schema["version"], "fingerprint": schema["fingerprint"], "end_frame": 2400},
                          "samples_sha256": release.digest(directory / "samples.jsonl.gz"),
                          "actions_sha256": release.digest(directory / "actions.jsonl.gz")}
                write(directory / "result.json", record)
                records.append((game["path"], game["replay_sha256"], "checkpoints_matched", json.dumps({
                    "status": "checkpoints_matched", "sha256": game["replay_sha256"], "native_checkpoints_sha256": "c" * 64})))
            cohort = {"games": games}
            write(old / "cohort.json", cohort)
            with closing(sqlite3.connect(config.audit)) as db:
                db.execute("CREATE TABLE replays (path TEXT,sha256 TEXT,detail TEXT)")
                db.executemany("INSERT INTO replays VALUES(?,?,?)", [(g["path"], g["replay_sha256"], "{}") for g in games])
                db.commit()
            with closing(sqlite3.connect(config.validation / "validation.sqlite")) as db:
                db.execute("CREATE TABLE results(path TEXT,sha256 TEXT,status TEXT,detail TEXT)")
                db.executemany("INSERT INTO results VALUES(?,?,?,?)", records)
                db.commit()
            write(config.validation / "summary.json", {"complete": True, "processed": 300, "total": 300,
                                                       "pending": 0, "matched": 300, "quarantined": 0})
            with patch.object(release, "verify_pins", return_value={}), patch.object(release, "verify", return_value={}), \
                    patch.object(release, "freeze", return_value=cohort), patch.object(release, "extract_game") as extract, \
                    patch.object(replay_pipeline.subprocess, "run") as process, patch("builtins.print"):
                result = release._run(config, {}, output, snapshot, False, 30)
                self.assertEqual(result["split_games"], {"train": 100, "validation": 100, "test": 100})
                self.assertFalse(result["training_started"])
                self.assertEqual(result["samples"], 15000)
                self.assertEqual(release.read_json(output / "release.json"), result)
                self.assertEqual(release._run(config, {}, output, snapshot, False, 30), result)
                extract.assert_not_called(); process.assert_not_called()
            # Cache verification hashes opaque test payloads but never interprets
            # their labels, and never calls the training/importer subprocess.


if __name__ == "__main__":
    unittest.main()
