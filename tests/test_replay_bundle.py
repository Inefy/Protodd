"""The source bundle must preserve every replay but restore only train by default."""

import hashlib
import json
from pathlib import Path
import tempfile
import unittest

from training.replay_bundle import create, restore, safe_name


class ReplayBundleTests(unittest.TestCase):
    def test_source_archive_preserves_splits_and_default_restore_is_train_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source"
            examples = {"PvP/train.rep": b"training replay",
                        "PvP/test.rep": b"sealed test replay",
                        "TvP/other.rep": b"unassigned replay"}
            for name, payload in examples.items():
                path = source / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(payload)
            release = {"games": [
                {"path": name, "split": split,
                 "replay_sha256": hashlib.sha256(examples[name]).hexdigest(),
                 "map_holdout": split == "test", "player_holdout": False}
                for name, split in (("PvP/train.rep", "train"),
                                    ("PvP/test.rep", "test"))]}
            manifest = root / "release.json"
            manifest.write_text(json.dumps(release), encoding="utf-8")
            bundle_root = root / "bundle"
            bundle = create(manifest, source, bundle_root)
            self.assertEqual(bundle["split_games"],
                             {"train": 1, "validation": 0, "test": 1,
                              "unassigned": 1})
            self.assertEqual(len(bundle["games"]), 3)
            output = root / "restored"
            self.assertEqual(restore(bundle_root, output)["restored"], 1)
            self.assertEqual((output / "PvP/train.rep").read_bytes(),
                             examples["PvP/train.rep"])
            self.assertFalse((output / "PvP/test.rep").exists())
            self.assertFalse((output / "TvP/other.rep").exists())
            self.assertEqual(restore(bundle_root, output, "source")["restored"], 2)
            self.assertEqual((output / "PvP/test.rep").read_bytes(),
                             examples["PvP/test.rep"])

    def test_rejects_changed_frozen_replay_and_unsafe_paths(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "source/PvP"
            source.mkdir(parents=True)
            (source / "train.rep").write_bytes(b"changed")
            manifest = root / "release.json"
            manifest.write_text(json.dumps({"games": [{
                "path": "PvP/train.rep", "split": "train",
                "replay_sha256": hashlib.sha256(b"original").hexdigest()}]}),
                encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "checksum mismatch"):
                create(manifest, root / "source", root / "bundle")
            with self.assertRaises(ValueError):
                safe_name("PvP/../test.rep")


if __name__ == "__main__":
    unittest.main()
