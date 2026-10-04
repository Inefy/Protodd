import csv
import importlib.util
import tempfile
import unittest
from unittest import mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "pvt_portfolio_validate", ROOT / "tools" / "pvt_portfolio_validate.py"
)
validator = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(validator)


class PortfolioValidatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.log = self.root / "Protodd.log"
        self.results = self.root / "trusted.csv"
        self.state = self.root / "state.json"
        self.history = self.root / "portfolio.csv"

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def event(match_id, outcome="win", arm="standard", map_name="Python", seed="7"):
        version = validator.VERSION
        selection = (
            "PVT_STRATEGY_SELECTION,id={arm},match_id={id},opponent=TerranBot,"
            "opponent_race=terran,map={map},strategy_version={version},source=adaptive-portfolio"
        ).format(arm=arm, id=match_id, map=map_name, version=version)
        result = (
            "PVT_STRATEGY_MATCH_RESULT,match_id={id},arm={arm},match_seed={seed},"
            "opponent=TerranBot,opponent_race=terran,map={map},strategy_version={version},"
            "outcome={outcome},externally_validated=0"
        ).format(id=match_id, arm=arm, seed=seed, map=map_name,
                 version=version, outcome=outcome)
        return selection + "\n" + result + "\n"

    @staticmethod
    def trusted(match_id, outcome="win", arm="standard", map_name="Python",
                completed="true", crashed="false", validated="true",
                opponent="TerranBot", opponent_race="terran", strategy_version=None):
        return {
            "match_id": match_id,
            "validator_id": "judge-" + match_id,
            "opponent": opponent,
            "opponent_race": opponent_race,
            "map": map_name,
            "strategy_version": strategy_version or validator.VERSION,
            "strategy": arm,
            "outcome": outcome,
            "completed": completed,
            "crashed": crashed,
            "externally_validated": validated,
        }

    def run_batch(self, events, result_rows):
        self.log.write_text(events, encoding="utf-8")
        with self.results.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=validator.TRUSTED_COLUMNS)
            writer.writeheader()
            writer.writerows(result_rows)
        return validator.validate_campaign(self.log, self.results, self.state, self.history)

    def test_unique_ids_deduplicate_replays_and_allow_same_seed_runs(self):
        rows = [self.trusted("run-a"), self.trusted("run-b")]
        first = self.run_batch(self.event("run-a") + self.event("run-b"), rows)
        self.assertEqual(first["added"], 2)
        self.assertEqual(first["duplicate"], 0)
        saved = self.history.read_text(encoding="utf-8")
        self.assertIn("TerranBot,Python,standard,2,0", saved)

        repeated = self.run_batch(self.event("run-a") + self.event("run-b"), rows)
        self.assertEqual(repeated["added"], 0)
        self.assertEqual(repeated["duplicate"], 2)
        self.assertEqual(self.history.read_text(encoding="utf-8"), saved)

        third = self.run_batch(self.event("run-c", seed="7"), [self.trusted("run-c")])
        self.assertEqual(third["added"], 1)
        self.assertIn("TerranBot,Python,standard,3,0", self.history.read_text(encoding="utf-8"))

    def test_mismatches_crashes_and_unpaired_results_are_excluded(self):
        events = self.event("good")
        events += self.event("wrong-map", map_name="Python")
        events += self.event("crash")
        events += self.event("wrong-arm")
        rows = [
            self.trusted("good"),
            self.trusted("wrong-map", map_name="Destination"),
            self.trusted("crash", completed="false", crashed="true"),
            self.trusted("wrong-arm", arm="economic-1gateway-observer"),
            self.trusted("no-log"),
        ]
        summary = self.run_batch(events, rows)
        self.assertEqual(summary["added"], 1)
        self.assertEqual(summary["rejected"], 4)
        self.assertIn("TerranBot,Python,standard,1,0", self.history.read_text(encoding="utf-8"))

    def test_changed_reuse_of_match_id_fails_without_mutating_outputs(self):
        self.run_batch(self.event("same-id"), [self.trusted("same-id")])
        old_state = self.state.read_bytes()
        old_history = self.history.read_bytes()
        variants = [
            self.trusted("same-id", outcome="loss"),
            self.trusted("same-id", crashed="true"),
            self.trusted("same-id", completed="false", crashed="true"),
            self.trusted("same-id", validated="false"),
            self.trusted("same-id", map_name="Destination"),
            self.trusted("same-id", arm="economic-1gateway-observer"),
            self.trusted("same-id", opponent="OtherBot"),
            self.trusted("same-id", opponent_race="protoss"),
            self.trusted("same-id", strategy_version="r3-siege-v2-pvt-portfolio-v1"),
        ]
        for changed in variants:
            with self.subTest(row=changed):
                with self.assertRaises(validator.ValidationError):
                    self.run_batch(self.event("same-id"), [changed])
                self.assertEqual(self.state.read_bytes(), old_state)
                self.assertEqual(self.history.read_bytes(), old_history)

    def test_conflicting_duplicate_ids_fail_before_race_or_build_filtering(self):
        original = self.trusted("duplicate")
        wrong_race = self.trusted("duplicate", opponent_race="protoss")
        wrong_build = self.trusted("duplicate", strategy_version="r3-siege-v2-pvt-portfolio-v1")
        for conflict in (wrong_race, wrong_build):
            with self.subTest(conflict=conflict):
                with self.assertRaises(validator.ValidationError):
                    self.run_batch(self.event("duplicate"), [original, conflict])
                self.assertFalse(self.state.exists())
                self.assertFalse(self.history.exists())

    def test_failed_history_replace_recovers_from_checksummed_state(self):
        original_write = validator._atomic_write

        def fail_history_write(path, data, maximum_bytes=validator.MAX_FILE_BYTES):
            if path == self.history:
                raise OSError("injected second replace failure")
            return original_write(path, data, maximum_bytes)

        with mock.patch.object(validator, "_atomic_write", side_effect=fail_history_write):
            with self.assertRaisesRegex(OSError, "injected second replace failure"):
                self.run_batch(self.event("recover"), [self.trusted("recover")])
        self.assertTrue(self.state.exists())
        self.assertFalse(self.history.exists())

        recovered = self.run_batch(self.event("recover"), [self.trusted("recover")])
        self.assertEqual(recovered["added"], 0)
        self.assertEqual(recovered["duplicate"], 1)
        self.assertIn("TerranBot,Python,standard,1,0", self.history.read_text(encoding="utf-8"))
        state = validator.json.loads(self.state.read_text(encoding="utf-8"))
        self.assertEqual(state["history_sha256"],
                         validator.hashlib.sha256(self.history.read_bytes()).hexdigest())
        self.assertEqual(state["state_sha256"], validator._state_fingerprint(state))

    def test_processed_id_corruption_fails_before_history_repair(self):
        self.run_batch(self.event("protected"), [self.trusted("protected")])
        state = validator.json.loads(self.state.read_text(encoding="utf-8"))
        state["processed_matches"].pop("protected")
        self.state.write_text(validator.json.dumps(state), encoding="utf-8")
        self.history.unlink()
        corrupted_state = self.state.read_bytes()

        with self.assertRaisesRegex(validator.ValidationError, "state checksum is corrupt"):
            self.run_batch(self.event("protected"), [self.trusted("protected")])

        self.assertEqual(self.state.read_bytes(), corrupted_state)
        self.assertFalse(self.history.exists())

    def test_legacy_state_with_unchecksummed_processed_ids_fails_closed(self):
        legacy = {
            "schema": validator.PREVIOUS_SCHEMA,
            "processed_matches": {"old": "0" * 64},
            "records": [],
            "history_sha256": validator.hashlib.sha256(
                validator._serialize_history([]).encode("utf-8")).hexdigest(),
        }
        self.state.write_text(validator.json.dumps(legacy), encoding="utf-8")
        before = self.state.read_bytes()

        with self.assertRaisesRegex(validator.ValidationError, "unchecksummed processed IDs"):
            self.run_batch(self.event("new"), [self.trusted("new")])

        self.assertEqual(self.state.read_bytes(), before)
        self.assertFalse(self.history.exists())

    def test_nonempty_history_without_dedup_state_fails_closed(self):
        existing = (
            "# PROTODD_PVT_PORTFOLIO 1\n"
            "# version,race,opponent,map,strategy,wins,losses\n"
            f"{validator.VERSION},terran,TerranBot,Python,standard,1,0\n"
        )
        self.history.write_text(existing, encoding="utf-8")
        before = self.history.read_bytes()
        with self.assertRaisesRegex(validator.ValidationError, "without validator dedup state"):
            self.run_batch(self.event("replay"), [self.trusted("replay")])
        self.assertFalse(self.state.exists())
        self.assertEqual(self.history.read_bytes(), before)

    def test_oversize_input_lines_are_rejected_before_state_changes(self):
        self.log.write_text("PVT_STRATEGY_SELECTION," + "x" * validator.MAX_LINE_BYTES,
                            encoding="utf-8")
        with self.results.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=validator.TRUSTED_COLUMNS)
            writer.writeheader()
        with self.assertRaises(validator.ValidationError):
            validator.validate_campaign(self.log, self.results, self.state, self.history)
        self.assertFalse(self.state.exists())
        self.assertFalse(self.history.exists())

    def test_oversized_state_is_rejected_before_json_parse(self):
        self.state.write_bytes(b" " * (validator.MAX_FILE_BYTES + 1))
        self.log.write_text("", encoding="utf-8")
        with self.results.open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=validator.TRUSTED_COLUMNS)
            writer.writeheader()
        with self.assertRaises(validator.ValidationError):
            validator.validate_campaign(self.log, self.results, self.state, self.history)
        self.assertFalse(self.history.exists())


if __name__ == "__main__":
    unittest.main()
