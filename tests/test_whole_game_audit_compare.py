import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_audit_compare import compare


class WholeGameAuditCompareTests(unittest.TestCase):
    def test_identical_examples_show_majority_and_nonmajority_recall(self):
        with tempfile.TemporaryDirectory() as directory:
            rows = [dict(game_id="g", frame=frame, true_kind=kind,
                         true_target_mode="position", target_mode_known=True,
                         predicted_target_mode="position", actor_correct=True,
                         supported_joint_kind="right_click",
                         supported_joint_target_mode="position", kind_rank=1)
                    for frame, kind in ((24, "right_click"), (48, "right_click"), (72, "build"))]
            reports = {}
            for name, predicted in (("majority", ["right_click"] * 3),
                                    ("model", ["right_click", "build", "build"])):
                payload = dict(sample_mode="natural", validation_identity_sha256="held-out",
                               checkpoint_sha256=name, rows=[dict(row, predicted_kind=value)
                                                              for row, value in zip(rows, predicted)])
                path = Path(directory) / (name + ".json")
                path.write_text(json.dumps(payload))
                reports[name] = path
            result = compare(reports)
            self.assertEqual(result["majority_correct"], 2)
            self.assertEqual(result["candidates"]["majority"]["kind_top1"], 2)
            self.assertEqual(result["candidates"]["majority"]["nonmajority_top1"], 0)
            self.assertEqual(result["candidates"]["model"]["nonmajority_top1"], 1)
            changed = json.loads(reports["model"].read_text())
            changed["rows"][0]["frame"] = 25
            reports["model"].write_text(json.dumps(changed))
            with self.assertRaisesRegex(ValueError, "same held-out examples"):
                compare(reports)


if __name__ == "__main__":
    unittest.main()
