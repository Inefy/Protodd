import gzip
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.analyze_dataset import analyze
from training.schema import load_schema


SCHEMA = load_schema()


def game(number, split="train"):
    return {"game_id": f"game:{number}", "replay_sha256": f"{number:064x}",
            "duplicate_group": f"group:{number}", "split": split, "races": ["P", "P"],
            "slots": [2, 5], "player_quality": ["qualified_ladder", "unknown"],
            "player_keys": [], "valid_through_frame": 96, "map_holdout": False,
            "player_holdout": False, "map_name": "Fixture"}


def sample(item, frame, action="wait", action_frame=None):
    features = [0.0] * len(SCHEMA["features"])
    features[0] = frame / 86400
    return {"game_id": item["game_id"], "perspective": 0, "frame": frame,
            "action_frame": frame if action_frame is None else action_frame,
            "features": features, "allowed_actions": list(SCHEMA["actions"]),
            "action": action, "confidence": 1}


def command(frame, action="train_probe", accepted=True, repeated=False, owner=2):
    return {"frame": frame, "owner": owner, "code": 31, "action": action,
            "producer": 123, "accepted": accepted, "repeated": repeated}


class DatasetAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name).resolve()
        self.source = self.root / "source"
        self.source.mkdir()

    def tearDown(self):
        self.temp.cleanup()

    def write_json(self, path, value):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(value), encoding="utf-8")

    def cohort(self, games, source_complete=True):
        self.write_json(self.source / "cohort.json", {
            "version": 1, "quality_rule": "exact-player source MMR >= 2000; pro tags unverified", "games": games})
        if source_complete:
            self.write_json(self.source / "status.json", {"cohort_games": len(games),
                "extracted_games": len(games), "quarantined_games": 0, "phase": "preparing_dataset"})

    def artifacts(self, item):
        directory = self.source / "games" / item["replay_sha256"]
        directory.mkdir(parents=True)
        samples = [sample(item, 0, "train_probe"), sample(item, 24), sample(item, 48, "build_pylon", 50)]
        actions = [command(0), command(1), command(2, "build_pylon", False, True),
                   command(3, "build_gateway", False), command(24, owner=5),
                   command(50, "build_pylon"), command(73, "train_dragoon"), command(96)]
        self.write_gzip(directory / "samples.jsonl.gz", samples)
        self.write_gzip(directory / "actions.jsonl.gz", actions)
        result = {"status": "extracted", "game_id": item["game_id"], "samples": 3,
            "counts": {"wait": 1, "train_probe": 1, "build_pylon": 1},
            "stats": {"schema": SCHEMA["version"], "fingerprint": SCHEMA["fingerprint"],
                      "end_frame": 96, "perspectives": [
                          {"perspective": 0, "slot": 2, "samples": 3, "positive_samples": 2,
                           "masked_windows": 1, "accepted_commands": 5, "rejected_commands": 1,
                           "repeated_build_commands": 1},
                          {"perspective": 1, "slot": 5, "samples": 20, "positive_samples": 12,
                           "masked_windows": 10, "accepted_commands": 1, "rejected_commands": 10,
                           "repeated_build_commands": 2}]},
            "samples_sha256": self.file_hash(directory / "samples.jsonl.gz"),
            "actions_sha256": self.file_hash(directory / "actions.jsonl.gz")}
        self.write_json(directory / "result.json", result)
        return directory, result, samples, actions

    @staticmethod
    def file_hash(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()

    @staticmethod
    def write_gzip(path, rows):
        with gzip.open(path, "wt", encoding="utf-8") as output:
            for row in rows:
                output.write(json.dumps(row) + "\n")

    def test_default_metadata_never_opens_test_or_validation_results_or_any_targets(self):
        items = [game(1), game(2, "validation"), game(3, "test")]
        self.cohort(items)
        self.artifacts(items[0])
        forbidden = {self.source / "games" / i["replay_sha256"] for i in items[1:]}
        original_open = Path.open

        def guarded_open(path, *args, **kwargs):
            if any(directory in path.parents for directory in forbidden) or path.suffix == ".gz":
                raise AssertionError(f"Forbidden target access: {path}")
            return original_open(path, *args, **kwargs)

        with patch.object(Path, "open", guarded_open):
            report = analyze(self.source, self.root / "analysis")
        self.assertEqual(report["inventory"]["splits"], {"train": 1, "validation": 1, "test": 1})
        self.assertEqual(report["scope"]["selected_games"], 1)
        self.assertEqual(report["recorded"]["action_counts"], {"wait": 1, "train_probe": 1, "build_pylon": 1})
        self.assertIsNone(report["labels"])
        self.assertFalse(report["training_ready"])

    def test_streaming_coverage_counts_simultaneous_mask_and_terminal_losses_separately(self):
        item = game(1)
        self.cohort([item])
        self.artifacts(item)
        report = analyze(self.source, self.root / "analysis", mode="labels")
        self.assertTrue(report["complete"])
        metrics = report["labels"]["metrics"]
        self.assertEqual(metrics["samples"], 3)
        self.assertEqual(metrics["positive_samples"], 2)
        self.assertEqual(metrics["accepted_commands"], 5)
        self.assertEqual(metrics["unretained_accepted_commands"], 3)
        self.assertEqual(metrics["additional_accepted_commands_in_retained_windows"], 1)
        self.assertEqual(metrics["accepted_commands_without_retained_window"], 2)
        self.assertEqual(metrics["accepted_commands_in_incomplete_terminal_window"], 1)
        self.assertEqual(metrics["repeated_build_commands"], 1)
        self.assertEqual(metrics["ignored_other_perspective_commands"], 1)
        self.assertEqual(report["recorded"]["metrics"]["masked_windows"]["total_observed"], 1)
        self.assertEqual(report["labels"]["retained_positive_fraction_of_accepted_commands"], .4)
        self.assertEqual(report["labels"]["action_delay_frames"], {"0": 1, "2": 1})
        self.assertEqual(json.loads((self.root / "analysis/progress.json").read_text())["stage"], "complete")

    def test_explicit_validation_inclusion_still_cannot_open_poisoned_final_test(self):
        items = [game(1), game(2, "validation"), game(3, "test")]
        self.cohort(items)
        for item in items[:2]:
            self.artifacts(item)
        forbidden = self.source / "games" / items[2]["replay_sha256"]
        original_open = Path.open

        def guarded_open(path, *args, **kwargs):
            if forbidden in path.parents:
                raise AssertionError("Final-test poison was read")
            return original_open(path, *args, **kwargs)

        with patch.object(Path, "open", guarded_open):
            report = analyze(self.source, self.root / "analysis", mode="labels", include_validation=True)
        self.assertEqual(report["labels"]["games_audited"], 2)
        self.assertEqual(set(report["labels"]["by_split"]), {"train", "validation"})
        self.assertFalse(report["scope"]["final_test_targets_opened"])

    def test_two_qualified_perspectives_keep_their_own_command_labels(self):
        item = game(1)
        item["player_quality"][1] = "qualified_ladder"
        self.cohort([item])
        directory, result, samples, _ = self.artifacts(item)
        other_sample = sample(item, 24, "train_probe")
        other_sample["perspective"] = 1
        samples.insert(2, other_sample)
        self.write_gzip(directory / "samples.jsonl.gz", samples)
        result["samples_sha256"] = self.file_hash(directory / "samples.jsonl.gz")
        result["samples"] = 4
        result["counts"]["train_probe"] = 2
        result["stats"]["perspectives"][1].update(samples=1, positive_samples=1, masked_windows=0,
                                                   accepted_commands=1, rejected_commands=0, repeated_build_commands=0)
        self.write_json(directory / "result.json", result)
        report = analyze(self.source, self.root / "analysis", mode="labels")
        self.assertTrue(report["complete"])
        self.assertEqual(report["labels"]["metrics"]["accepted_commands"], 6)
        self.assertEqual(report["labels"]["metrics"]["positive_samples"], 3)
        self.assertEqual(report["inventory"]["qualified_perspectives"], {"train": 2})

    def test_bounded_pilot_and_missing_results_are_partial_not_zero_evidence(self):
        items = [game(1), game(2), game(3)]
        self.cohort(items, source_complete=False)
        self.artifacts(items[0])
        report = analyze(self.source, self.root / "pilot", max_games=2)
        self.assertTrue(report["audit_completed"])
        self.assertFalse(report["complete"])
        self.assertFalse(report["scope_complete"])
        self.assertIsNone(report["source_complete"])
        self.assertTrue(report["scope"]["bounded_pilot"])
        self.assertEqual(report["selected_result_statuses"]["missing_artifact"], 1)
        self.assertEqual(report["scope"]["selected_games"], 2)

    def test_unknown_statistics_remain_unknown_and_missing_hashes_do_not_certify_labels(self):
        item = game(1)
        self.cohort([item])
        directory, result, _, _ = self.artifacts(item)
        result.pop("stats")
        result.pop("actions_sha256")
        self.write_json(directory / "result.json", result)
        report = analyze(self.source, self.root / "analysis", mode="labels")
        self.assertEqual(report["recorded"]["metrics"]["masked_windows"]["games_without_evidence"], 1)
        self.assertEqual(report["labels"]["games_without_verified_hashes"], 1)
        self.assertFalse(report["complete"])

    def test_identity_and_causality_corruption_excludes_the_entire_game(self):
        mutations = {
            "wrong_game": lambda row: row.update(game_id="game:wrong"),
            "future_feature": lambda row: row["features"].__setitem__(0, 400 / 86400),
            "future_label": lambda row: row.update(action_frame=24),
            "unqualified_perspective": lambda row: row.update(perspective=1),
            "wrong_first_action": lambda row: row.update(action="build_pylon"),
        }
        for name, mutate in mutations.items():
            with self.subTest(name=name):
                source = self.source
                self.source = self.root / name
                self.source.mkdir()
                item = game(1)
                self.cohort([item])
                directory, result, rows, _ = self.artifacts(item)
                mutate(rows[0])
                self.write_gzip(directory / "samples.jsonl.gz", rows)
                result["samples_sha256"] = self.file_hash(directory / "samples.jsonl.gz")
                self.write_json(directory / "result.json", result)
                report = analyze(self.source, self.root / (name + "-analysis"), mode="labels")
                self.assertEqual(report["selected_result_statuses"]["invalid_artifact"], 1)
                self.assertEqual(report["labels"]["games_audited"], 0)
                self.assertFalse(report["scope_complete"])
                self.source = source

    def test_hash_mismatch_and_missing_action_stream_report_actionable_failures(self):
        item = game(1)
        self.cohort([item])
        directory, result, _, _ = self.artifacts(item)
        result["samples_sha256"] = "0" * 64
        self.write_json(directory / "result.json", result)
        report = analyze(self.source, self.root / "mismatch", mode="labels")
        self.assertIn("hash mismatch", report["error_examples"][0]["error"])
        (directory / "actions.jsonl.gz").unlink()
        report = analyze(self.source, self.root / "missing", mode="labels")
        self.assertEqual(report["selected_result_statuses"]["missing_artifact"], 1)
        self.assertEqual(report["error_examples"][0]["error"], "actions.jsonl.gz")

    def test_nonchronological_command_stream_is_not_used_for_coverage(self):
        item = game(1)
        self.cohort([item])
        directory, result, _, actions = self.artifacts(item)
        actions.append(command(48))
        self.write_gzip(directory / "actions.jsonl.gz", actions)
        result["actions_sha256"] = self.file_hash(directory / "actions.jsonl.gz")
        self.write_json(directory / "result.json", result)
        report = analyze(self.source, self.root / "analysis", mode="labels")
        self.assertEqual(report["labels"]["games_audited"], 0)
        self.assertEqual(report["selected_result_statuses"]["invalid_artifact"], 1)

    def test_duplicate_group_leakage_fails_before_reading_any_game(self):
        first, heldout = game(1), game(2, "test")
        heldout["duplicate_group"] = first["duplicate_group"]
        self.cohort([first, heldout])
        with self.assertRaisesRegex(ValueError, "crosses splits"):
            analyze(self.source, self.root / "analysis", mode="labels")
        progress = json.loads((self.root / "analysis/progress.json").read_text())
        self.assertEqual(progress["status"], "failed")

    def test_result_slot_identity_is_validated(self):
        item = game(1)
        self.cohort([item])
        directory, result, _, _ = self.artifacts(item)
        result["stats"]["perspectives"][0]["slot"] = 5
        self.write_json(directory / "result.json", result)
        report = analyze(self.source, self.root / "analysis")
        self.assertEqual(report["selected_result_statuses"]["invalid_artifact"], 1)
        self.assertIn("slot identity", report["error_examples"][0]["error"])

    def test_quarantine_and_incomplete_source_are_not_silently_treated_as_ready(self):
        items = [game(1), game(2)]
        self.cohort(items)
        self.artifacts(items[0])
        self.write_json(self.source / "games" / items[1]["replay_sha256"] / "result.json",
                        {"status": "quarantined", "game_id": items[1]["game_id"], "error": "Playback validation did not pass"})
        self.write_json(self.source / "status.json", {"cohort_games": 2, "extracted_games": 1,
                        "quarantined_games": 0, "phase": "validating_and_extracting"})
        report = analyze(self.source, self.root / "analysis")
        self.assertTrue(report["scope_complete"])
        self.assertFalse(report["source_complete"])
        self.assertFalse(report["complete"])
        self.assertEqual(report["exclusions"], {"Playback validation did not pass": 1})

    def test_refuses_to_write_inside_source_or_overwrite_an_analysis(self):
        self.cohort([game(1)])
        with self.assertRaisesRegex(ValueError, "outside"):
            analyze(self.source, self.source / "analysis")
        output = self.root / "existing"
        output.mkdir()
        marker = output / "marker"
        marker.write_text("keep")
        with self.assertRaises(FileExistsError):
            analyze(self.source, output)
        self.assertEqual(marker.read_text(), "keep")

    def test_cli_has_no_final_test_option(self):
        result = subprocess.run([sys.executable, "-m", "training.analyze_dataset", "--help"],
                                cwd=Path(__file__).resolve().parents[1], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn("--include-validation", result.stdout)
        self.assertNotIn("--include-test", result.stdout)


if __name__ == "__main__":
    unittest.main()
