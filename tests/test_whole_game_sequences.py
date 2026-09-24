import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_labels import LABEL_SCHEMA
from training.whole_game_pilot import SCHEMA
from training.whole_game_sequences import trajectory


class WholeGameSequenceTests(unittest.TestCase):
    def test_timing_window_and_censoring(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "summary.json").write_text(json.dumps(dict(schema=SCHEMA, complete=True,
                                                                  valid_through_frame=48)))
            rows = [(0, 0, "cadence"), (1, 23, "before_command"),
                    (2, 24, "cadence"), (3, 47, "before_command"), (4, 48, "cadence")]
            (root / "observations.jsonl").write_text("".join(json.dumps(dict(schema=SCHEMA,
                sequence=sequence, frame=frame, reason=reason, perspective=0)) + "\n"
                for sequence, frame, reason in rows))
            labels = root / "labels.jsonl"
            labels.write_text("".join(json.dumps(dict(schema=LABEL_SCHEMA,
                observation_sequence=sequence, frame=frame, perspective=0)) + "\n"
                for sequence, frame in ((1, 23), (3, 47))))
            result = list(trajectory(root, labels))
            self.assertEqual([supervision["event"] for _, supervision in result],
                             [1, None, 1, None, None])
            self.assertEqual([supervision["update_memory"] for _, supervision in result],
                             [True, False, True, False, True])
            self.assertEqual(result[1][1]["action"]["frame"], 23)
            labels.write_text(json.dumps(dict(schema=LABEL_SCHEMA,
                                              observation_sequence=999, frame=23, perspective=0)))
            with self.assertRaisesRegex(ValueError, "lack source"):
                list(trajectory(root, labels))


if __name__ == "__main__":
    unittest.main()
