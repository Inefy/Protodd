import csv
import tempfile
import unittest
from pathlib import Path

from training.whole_game_execution_audit import audit, load_events, summarize


COLUMNS = [
    "attempt_id", "proposal_frame", "slot", "due_frame", "frame", "actor_token",
    "actor_ordinal", "actor_count", "intent_kind", "target_mode", "target_entity",
    "target_x", "target_y", "stage", "outcome", "reason",
]


def event(attempt, actor, ordinal, stage, outcome, reason="", frame=100):
    return dict(attempt_id=attempt, proposal_frame=96, slot=0, due_frame=100,
                frame=frame, actor_token=actor, actor_ordinal=ordinal, actor_count=1,
                intent_kind=1, target_mode=0, target_entity=-1, target_x=-1,
                target_y=-1, stage=stage, outcome=outcome, reason=reason)


class WholeGameExecutionAuditTests(unittest.TestCase):
    def test_separates_legality_arbitration_api_and_native_observation(self):
        rows = [
            event(1, 11, 0, "proposal", "emitted"),
            event(1, 11, 0, "legality", "legal"),
            event(1, 11, 0, "arbitration", "selected"),
            event(1, 11, 0, "api", "accepted"),
            event(1, 11, 0, "execution", "observed", "native-last-command-matched", 101),
            event(2, 12, 0, "proposal", "emitted"),
            event(2, 12, 0, "legality", "legal"),
            event(2, 12, 0, "arbitration", "deferred", "actor-already-claimed"),
            event(2, 12, 0, "arbitration", "selected", frame=102),
            event(2, 12, 0, "api", "accepted", frame=102),
            event(2, 12, 0, "execution", "unobserved", "command-not-matched", 126),
            event(3, 13, 0, "proposal", "emitted"),
            event(3, 13, 0, "legality", "rejected", "target-not-visible"),
            event(4, 14, 0, "proposal", "emitted"),
            event(4, 14, 0, "legality", "legal"),
            event(4, 14, 0, "arbitration", "selected"),
            event(4, 14, 0, "api", "accepted"),
            event(4, 14, 0, "execution", "censored", "target-no-longer-exists", 104),
            event(5, 15, 0, "proposal", "emitted"),
            event(5, 15, 0, "legality", "legal"),
            event(5, 15, 0, "arbitration", "selected"),
            event(5, 15, 0, "api", "rejected", 'Unit "Busy", retry later'),
        ]

        report = summarize(rows)

        self.assertEqual(report["actor_attempts"], {
            "proposed": 5,
            "legal": 4,
            "legality_rejected": 1,
            "selected": 4,
            "api_accepted": 3,
            "api_rejected": 1,
            "execution_observed": 1,
            "execution_unobserved": 1,
            "execution_censored": 1,
            "accepted_pending_execution": 0,
            "legal_not_selected": 0,
            "selected_not_accepted": 1,
            "selected_awaiting_api": 0,
        })
        self.assertEqual(report["rates"]["proposal_to_legal"], 0.8)
        self.assertEqual(report["rates"]["observed_among_uncensored_terminal"], 0.5)
        self.assertIn("legality:rejected", report["event_counts"])
        self.assertTrue(any(item["reason"] == 'Unit "Busy", retry later'
                            for item in report["loss_reasons"]))
        self.assertIn("does not prove", report["measurement_note"])

    def test_loads_escaped_reasons_and_hashes_exact_input(self):
        rows = [event(7, 21, 0, "proposal", "emitted")]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "WholeGame-actions.csv"
            with path.open("w", encoding="utf-8", newline="") as output:
                writer = csv.DictWriter(output, fieldnames=COLUMNS)
                writer.writeheader()
                writer.writerows(rows)
            loaded = load_events(path)
            self.assertEqual(loaded[0]["attempt_id"], 7)
            self.assertEqual(audit(path)["event_rows"], 1)
            self.assertEqual(len(audit(path)["input_sha256"]), 64)

    def test_rejects_a_truncated_or_malformed_csv(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "bad.csv"
            path.write_text("attempt_id,stage,outcome\n1,proposal,emitted\n", encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "missing required columns"):
                load_events(path)


if __name__ == "__main__":
    unittest.main()
