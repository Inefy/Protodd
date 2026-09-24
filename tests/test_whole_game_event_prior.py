import gzip
import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_event_prior import count_game


class EventPriorTests(unittest.TestCase):
    def test_counts_distinct_complete_cadence_windows(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with gzip.open(root / "summary.json.gz", "wt", encoding="utf8") as stream:
                json.dump(dict(complete=True, valid_through_frame=72), stream)
            with gzip.open(root / "imitation-labels.jsonl.gz", "wt", encoding="utf8") as stream:
                for frame in (5, 6, 24, 70, 72):
                    stream.write(json.dumps(dict(frame=frame)) + "\n")
            self.assertEqual(count_game(root), (3, 3))
            self.assertEqual(count_game(root, window=36), (2, 2))


if __name__ == "__main__":
    unittest.main()
