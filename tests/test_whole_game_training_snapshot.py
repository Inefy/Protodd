import hashlib
import json
from pathlib import Path
import tempfile
import unittest

import torch

from training.whole_game_training_snapshot import snapshot


class TrainingSnapshotTests(unittest.TestCase):
    def test_source_hash_and_completed_group_are_required(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = dict(source_identity_sha256="release", groups=[{}], epochs=1)
            run_path = root / "run.json"
            run_path.write_text(json.dumps(run), encoding="utf8")
            digest = hashlib.sha256(json.dumps(run, sort_keys=True).encode()).hexdigest()
            resume_path = root / "resume.pt"
            torch.save(dict(spec_digest=digest, next_group=1,
                            model={"weight": torch.tensor([3.0])}), resume_path)
            report = snapshot(run_path, resume_path, root / "diagnostic")
            self.assertTrue(report["diagnostic_only"])
            self.assertEqual(report["groups_completed"], 1)
            teacher = torch.load(root / "diagnostic" / "teacher.pt",
                                 map_location="cpu", weights_only=True)
            self.assertEqual(teacher["source_identity_sha256"], "release")
            self.assertEqual(teacher["state_dict"]["weight"].item(), 3)
            torch.save(dict(spec_digest="wrong", next_group=1, model={}), resume_path)
            with self.assertRaisesRegex(ValueError, "does not match"):
                snapshot(run_path, resume_path, root / "rejected")


if __name__ == "__main__":
    unittest.main()
