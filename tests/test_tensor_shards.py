"""Tiny end-to-end data equivalence, test isolation and causal resume checks."""
from collections import Counter
import copy
import gzip
import hashlib
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.pack_dataset import _Progress, _Writer, pack, wait_for_release
from training.prepare import validate_manifest, validate_sample
from training.schema import load_schema, sha256
from training.shards import SequenceSampler, TensorShards

SCHEMA = load_schema()


def fixture(directory):
    root = Path(directory)
    games_root = root / "games"
    games_root.mkdir()
    games, rows_by_game = [], {}
    specifications = [
        ("a", "train", ["P", "P"], (7, 3)),
        ("b", "train", ["P", "Z"], (29, 0)),
        ("c", "train", ["P", "T"], (4, 0)),
        ("v", "validation", ["P", "T"], (8, 0)),
        ("sealed", "test", ["P", "Z"], (1, 0)),
    ]
    for gid, split, races, sizes in specifications:
        digest = hashlib.sha256(gid.encode()).hexdigest()
        game = dict(game_id=gid, duplicate_group=gid, split=split,
                    replay_sha256=digest, races=races,
                    player_quality=["verified_pro", "qualified_ladder"], valid_through_frame=24000)
        games.append(game)
        target = games_root / digest
        target.mkdir()
        if split == "test":
            # No result.json and unreadable sample syntax: even inspecting this
            # game's source metadata would fail, not merely fitting its tensors.
            (target / "samples.jsonl.gz").write_bytes(b"POISONED FINAL TEST")
            continue
        rows = []
        for perspective, size in enumerate(sizes):
            for i in range(size):
                frame = i * 24 + (48 if i >= 2 else 0)
                features = [0.0] * len(SCHEMA["features"])
                features[0] = frame / 86400
                features[1] = (i % 3) / 3
                rows.append(dict(game_id=gid, perspective=perspective, frame=frame,
                                 action_frame=frame + (3 if i % 2 else 0), features=features,
                                 allowed_actions=["wait", "train_probe", "build_pylon"],
                                 action="train_probe" if i % 2 else "wait", confidence=0.75))
        rows.sort(key=lambda row: (row["frame"], row["perspective"]))
        rows_by_game[gid] = rows
        write_rows(target, gid, rows)
    manifest = dict(schema=SCHEMA["version"], fingerprint=SCHEMA["fingerprint"],
                    source="synthetic_test", extractor="fixture", audit_id="fixture-only",
                    validation=dict(playback=True, perspective=True, actions=True), games=games)
    path = root / "source-manifest.json"
    path.write_text(json.dumps(manifest), encoding="utf-8")
    return path, games_root, rows_by_game


def write_rows(target, gid, rows):
    sample = target / "samples.jsonl.gz"
    with gzip.open(sample, "wt", encoding="utf-8") as stream:
        for row in rows:
            stream.write(json.dumps(row) + "\n")
    result = dict(status="extracted", game_id=gid, samples=len(rows), samples_sha256=sha256(sample))
    (target / "result.json").write_text(json.dumps(result), encoding="utf-8")


def freeze_human_fixture(manifest_path, games_root):
    manifest = json.loads(manifest_path.read_text())
    manifest["source"] = "human_replay"
    manifest_path.write_text(json.dumps(manifest), encoding="utf-8")
    sources = {}
    for game in manifest["games"]:
        directory = games_root / game["replay_sha256"]
        if game["split"] == "test":
            # Retain deliberately absent/unreadable test artifacts. Only these
            # frozen metadata claims may be touched by the training packer.
            result_hash = samples_hash = actions_hash = "0" * 64
        else:
            result_path = directory / "result.json"
            result = json.loads(result_path.read_text())
            result["actions_sha256"] = "a" * 64
            result_path.write_text(json.dumps(result), encoding="utf-8")
            result_hash, samples_hash, actions_hash = sha256(result_path), result["samples_sha256"], result["actions_sha256"]
        sources[game["game_id"]] = dict(directory=str(directory.resolve()), result_sha256=result_hash,
                                       samples_sha256=samples_hash, actions_sha256=actions_hash)
    source_path = manifest_path.parent / "sources.json"
    source_path.write_text(json.dumps(dict(format_version=1, games=sources)), encoding="utf-8")
    release = dict(format_version=1, complete=True, stage="extraction_release", training_started=False,
                   automatic_training=False, manifest_sha256=sha256(manifest_path),
                   sources_sha256=sha256(source_path))
    path = manifest_path.parent / "release.json"
    path.write_text(json.dumps(release), encoding="utf-8")
    return path


class TensorShardTests(unittest.TestCase):
    def test_exact_importer_values_and_poisoned_test_never_opened(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest_path, games_root, rows_by_game = fixture(directory)
            output = Path(directory) / "packed"
            opened = []
            original_open = gzip.open
            def tracked_open(path, *args, **kwargs):
                opened.append(Path(path))
                return original_open(path, *args, **kwargs)
            with patch("training.pack_dataset.gzip.open", side_effect=tracked_open):
                info = pack(manifest_path, games_root, output, shard_rows=5, allow_synthetic=True)
            self.assertEqual(len(opened), 4)
            self.assertEqual(info["excluded_test_games"], ["sealed"])
            self.assertFalse(info["test_samples_opened"])
            self.assertEqual(info["counts"], {"train": 43, "validation": 8})
            game_map = validate_manifest(json.loads(manifest_path.read_text()), SCHEMA, True)
            expected = [validate_sample(row, game_map, SCHEMA)
                        for gid in sorted(rows_by_game)
                        for row in sorted(rows_by_game[gid], key=lambda r: (r["perspective"], r["frame"]))]
            with TensorShards(output) as data:
                actual = data.read_rows(0, data.row_count)
                np.testing.assert_array_equal(actual["features"], np.stack([
                    np.frombuffer(r[4], dtype="<f4") for r in expected]))
                for key, position, dtype in [("perspectives", 1, "u1"), ("frames", 2, "<i4"),
                                              ("action_frames", 3, "<i4"), ("masks", 5, "<u8"),
                                              ("labels", 6, "u1"), ("weights", 7, "<f4")]:
                    np.testing.assert_array_equal(actual[key], np.asarray([r[position] for r in expected], dtype=dtype))
                self.assertEqual(actual["features"].dtype, np.dtype("<f4"))
                self.assertGreater(len(info["shards"]), 2)
            self.assertFalse(output.with_name(output.name + ".partial").exists())
            status = json.loads(output.with_name(output.name + ".status.json").read_text())
            self.assertEqual(status["phase"], "complete")
            self.assertEqual(status["samples_processed"], len(expected))
            self.assertEqual(status["estimated_remaining_seconds"], 0)
            self.assertFalse((output / "status.json").exists())
            second = pack(manifest_path, games_root, Path(directory) / "packed-again",
                          shard_rows=5, allow_synthetic=True)
            self.assertEqual(info, second)
            with self.assertRaisesRegex(ValueError, "exists"):
                pack(manifest_path, games_root, output, allow_synthetic=True)

    def test_causal_windows_keep_gaps_burn_in_and_player_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            output = Path(directory) / "packed"
            pack(manifest, games_root, output, shard_rows=3, allow_synthetic=True)
            with TensorShards(output) as data:
                sample = data.sequence(0, start=2, length=3, burn_in=1)
                np.testing.assert_array_equal(sample["frames"], [24, 96, 120, 144])
                np.testing.assert_array_equal(sample["delta_frames"], [24, 72, 24, 24])
                np.testing.assert_array_equal(sample["gap_mask"], [False, True, False, False])
                np.testing.assert_array_equal(sample["loss_mask"], [False, True, True, True])
                np.testing.assert_array_equal(sample["reset_mask"], [True, False, False, False])
                self.assertFalse(sample["is_game_start"])
                self.assertTrue(np.all(sample["perspectives"] == 0))
                end = data.sequence(0, start=6, length=20, burn_in=2)
                self.assertEqual(len(end["frames"]), 3)
                self.assertEqual(int(end["loss_mask"].sum()), 1)
                other = data.sequence(1, start=0, length=10, burn_in=8)
                self.assertTrue(other["is_game_start"])
                self.assertEqual(other["burn_in"], 0)
                self.assertEqual(other["delta_frames"][0], 0)
                self.assertTrue(np.all(other["perspectives"] == 1))
                with self.assertRaises(ValueError):
                    data.sequence(0, start=7, length=1)

    def test_sampler_resume_uses_consumed_position_not_prefetch_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            output = Path(directory) / "packed"
            pack(manifest, games_root, output, allow_synthetic=True)
            with TensorShards(output) as data:
                sampler = SequenceSampler(data, seed=7, sequence_length=4, burn_in=2)
                uninterrupted = [sampler.sample_at(i) for i in range(12)]
                # Deliberate out-of-order prefetch must leave consumed=0.
                for i in (9, 4, 11, 5):
                    sampler.sample_at(i)
                self.assertEqual(sampler.state_dict()["consumed_samples"], 0)
                for i in range(4):
                    sampler.acknowledge(i)
                state = json.loads(json.dumps(sampler.state_dict()))
                resumed = SequenceSampler(data, seed=7, sequence_length=4, burn_in=2)
                resumed.load_state_dict(state)
                for i in range(resumed.consumed, 12):
                    actual = resumed.sample_at(i)
                    self.assertEqual(actual["split"], "train")
                    self.assertEqual(actual["game_id"], uninterrupted[i]["game_id"])
                    np.testing.assert_array_equal(actual["features"], uninterrupted[i]["features"])
                    np.testing.assert_array_equal(actual["loss_mask"], uninterrupted[i]["loss_mask"])
                    resumed.acknowledge(i)
                with self.assertRaisesRegex(ValueError, "consumed order"):
                    resumed.acknowledge(11)
                with self.assertRaisesRegex(ValueError, "configuration"):
                    SequenceSampler(data, seed=8, sequence_length=4, burn_in=2).load_state_dict(state)

    def test_sampler_balances_games_and_perspectives_not_row_count(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            output = Path(directory) / "packed"
            pack(manifest, games_root, output, allow_synthetic=True)
            with TensorShards(output) as data:
                sampler = SequenceSampler(data, seed=123, sequence_length=1, balance_matchups=False)
                counts, perspectives = Counter(), Counter()
                for i in range(1800):
                    sample = sampler.sample_at(i)
                    counts[sample["game_id"]] += 1
                    if sample["game_id"] == "a":
                        perspectives[sample["perspective"]] += 1
                self.assertEqual(set(counts), {"a", "b", "c"})
                for count in counts.values():
                    self.assertTrue(500 < count < 700, counts)
                self.assertLess(abs(perspectives[0] - perspectives[1]), 100)

    def test_source_corruption_and_invalid_rows_leave_no_completed_release(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, rows = fixture(directory)
            output = Path(directory) / "invalid"
            game = json.loads(manifest.read_text())["games"][0]
            bad = copy.deepcopy(rows["a"])
            bad[1]["action_frame"] = bad[1]["frame"] + 24
            write_rows(games_root / game["replay_sha256"], "a", bad)
            with self.assertRaisesRegex(ValueError, "24-frame"):
                pack(manifest, games_root, output, allow_synthetic=True)
            self.assertFalse(output.exists())
            self.assertTrue(output.with_name(output.name + ".partial").exists())
            with self.assertRaisesRegex(ValueError, "exists"):
                pack(manifest, games_root, output, allow_synthetic=True)
            with self.assertRaisesRegex(ValueError, "unfinished"):
                TensorShards(output.with_name(output.name + ".partial"))
            write_rows(games_root / game["replay_sha256"], "a", rows["a"])
            with (games_root / game["replay_sha256"] / "samples.jsonl.gz").open("ab") as stream:
                stream.write(b"changed")
            with self.assertRaisesRegex(ValueError, "checksum"):
                pack(manifest, games_root, Path(directory) / "corrupt-source", allow_synthetic=True)

    def test_corrupted_shards_and_incomplete_manifest_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            output = Path(directory) / "packed"
            info = pack(manifest, games_root, output, allow_synthetic=True)
            path = output / "manifest.json"
            incomplete = {**info, "complete": False}
            path.write_text(json.dumps(incomplete), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "incomplete"):
                TensorShards(output)
            path.write_text(json.dumps(info), encoding="utf-8")
            shard = output / info["shards"][0]["features"]["file"]
            with shard.open("r+b") as stream:
                stream.seek(-1, 2)
                value = stream.read(1)
                stream.seek(-1, 2)
                stream.write(bytes([value[0] ^ 1]))
            with self.assertRaisesRegex(ValueError, "checksum"):
                TensorShards(output)

    def test_human_release_binding_rejects_self_consistent_source_edits(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, rows = fixture(directory)
            release = freeze_human_fixture(manifest, games_root)
            output = Path(directory) / "human-packed"
            info = pack(manifest, games_root, output, release=release)
            self.assertEqual(info["extraction_release"]["release_sha256"], sha256(release))
            self.assertFalse(info["test_samples_opened"])
            bad_rows = copy.deepcopy(rows["a"])
            bad_rows[0]["features"][1] = 0.9
            game = json.loads(manifest.read_text())["games"][0]
            # Both compressed data and its result checksum now agree with one
            # another, but must still disagree with the frozen release index.
            write_rows(games_root / game["replay_sha256"], "a", bad_rows)
            with self.assertRaisesRegex(ValueError, "differs from frozen release"):
                pack(manifest, games_root, Path(directory) / "edited")

    def test_release_checksums_completion_and_human_binding_are_mandatory(self):
        for mutation, expected in (("sources", "sources checksum"), ("manifest", "manifest checksum"),
                                   ("active", "training held"), ("incomplete", "complete frozen"),
                                   ("missing", "release.json")):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                manifest, games_root, _ = fixture(directory)
                release = freeze_human_fixture(manifest, games_root)
                if mutation == "sources":
                    (manifest.parent / "sources.json").write_text("{}", encoding="utf-8")
                elif mutation == "manifest":
                    with manifest.open("a") as stream:
                        stream.write("\n")
                elif mutation in ("active", "incomplete"):
                    value = json.loads(release.read_text())
                    value["training_started" if mutation == "active" else "complete"] = mutation == "active"
                    release.write_text(json.dumps(value), encoding="utf-8")
                else:
                    release.unlink()
                with self.assertRaisesRegex((ValueError, FileNotFoundError), expected):
                    pack(manifest, games_root, Path(directory) / "rejected", allow_synthetic=True)

    def test_result_changes_during_packing_prevent_completion(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            freeze_human_fixture(manifest, games_root)
            game = json.loads(manifest.read_text())["games"][0]
            result_path = games_root / game["replay_sha256"] / "result.json"
            original = _Writer.append
            changed = False
            def changing_append(writer, rows):
                nonlocal changed
                original(writer, rows)
                if not changed:
                    with result_path.open("a") as stream:
                        stream.write("\n")
                    changed = True
            output = Path(directory) / "race"
            with patch("training.pack_dataset._Writer.append", changing_append):
                with self.assertRaisesRegex(ValueError, "result changed"):
                    pack(manifest, games_root, output)
            self.assertFalse(output.exists())
            status = json.loads(output.with_name(output.name + ".status.json").read_text())
            self.assertEqual(status["phase"], "failed")

    def test_progress_updates_on_fifteen_second_cadence(self):
        with tempfile.TemporaryDirectory() as directory:
            with patch("training.pack_dataset.time.monotonic", side_effect=[0.0, 0.0, 5.0, 16.0]):
                progress = _Progress(Path(directory) / "packed")
                progress.update("packing", samples_total=100, samples_processed=0)
                progress.update(samples_processed=20)
                self.assertEqual(json.loads(progress.path.read_text())["samples_processed"], 0)
                progress.update(samples_processed=40)
                actual = json.loads(progress.path.read_text())
                self.assertEqual(actual["samples_processed"], 40)
                self.assertEqual(actual["elapsed_seconds"], 16.0)
                self.assertEqual(actual["estimated_remaining_seconds"], 24.0)

    def test_wait_for_release_pending_then_ready_without_packing(self):
        with tempfile.TemporaryDirectory() as directory:
            manifest, games_root, _ = fixture(directory)
            release = freeze_human_fixture(manifest, games_root)
            frozen_bytes = release.read_bytes()
            release.unlink()
            output = Path(directory) / "deferred"
            def finish_release(seconds):
                self.assertEqual(seconds, 15)
                status = json.loads(output.with_name(output.name + ".status.json").read_text())
                self.assertEqual(status["phase"], "waiting_for_extraction_release")
                self.assertFalse(status["training_started"])
                release.write_bytes(frozen_bytes)
            with patch("training.pack_dataset.time.sleep", side_effect=finish_release) as sleep:
                self.assertEqual(wait_for_release(manifest, output), release)
                sleep.assert_called_once_with(15)
            self.assertFalse(output.exists())
            self.assertFalse(output.with_name(output.name + ".partial").exists())
            status = json.loads(output.with_name(output.name + ".status.json").read_text())
            self.assertEqual(status["phase"], "extraction_release_ready")

    def test_wait_surfaces_failure_and_never_overrides_incomplete_release(self):
        for condition in ("failure", "incomplete", "existing", "partial"):
            with self.subTest(condition=condition), tempfile.TemporaryDirectory() as directory:
                manifest, games_root, _ = fixture(directory)
                release = freeze_human_fixture(manifest, games_root)
                output = Path(directory) / "deferred"
                if condition == "failure":
                    (release.parent / "failure.json").write_text(json.dumps({"error": "fidelity gate failed"}))
                    expected = "fidelity gate failed"
                elif condition == "incomplete":
                    value = json.loads(release.read_text())
                    value["complete"] = False
                    release.write_text(json.dumps(value), encoding="utf-8")
                    expected = "incomplete or invalid"
                else:
                    (output if condition == "existing" else output.with_name(output.name + ".partial")).mkdir()
                    expected = "exists"
                before = release.read_bytes()
                with patch("training.pack_dataset.time.sleep") as sleep:
                    with self.assertRaisesRegex(ValueError, expected):
                        wait_for_release(manifest, output)
                    sleep.assert_not_called()
                self.assertEqual(release.read_bytes(), before)


if __name__ == "__main__":
    unittest.main()
