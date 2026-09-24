import json
import math
from pathlib import Path
import tempfile
import unittest

from training.whole_game_event_calibration import calibrate, corrected_probability


class EventCalibrationTests(unittest.TestCase):
    def test_log_odds_shift_and_release_identity(self):
        self.assertAlmostEqual(corrected_probability(0.5, math.log(3)), 0.75)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            event = root / "event.json"
            prior = root / "prior.json"
            event.write_text(json.dumps(dict(
                schema="protodd-whole-game-event-audit-v1",
                training_identity_sha256="train", validation_identity_sha256="validation",
                checkpoint_sha256="checkpoint", overall={"windows": 1},
                rows=[dict(game_id="g", matchup="PvT", frame=0, event=1,
                           probability=0.5)])))
            prior.write_text(json.dumps(dict(
                schema="protodd-whole-game-event-prior-v1", train_only=True,
                source_identity_sha256="wrong", logit_prior_correction=math.log(3))))
            with self.assertRaisesRegex(ValueError, "incompatible"):
                calibrate(event, prior, root / "rejected.json")


if __name__ == "__main__":
    unittest.main()
