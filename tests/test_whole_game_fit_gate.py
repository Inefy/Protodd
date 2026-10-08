import hashlib
import json
from pathlib import Path
import tempfile
import unittest
from argparse import Namespace
from unittest.mock import patch

from training.whole_game_fit_gate import (
    AUDIT_SOURCE_FILES,
    MATCHUPS,
    PARITY_SCHEMA,
    PREDECLARED_LIMITS,
    SCHEMA,
    verify_pre_fit_gate,
)
from training import (whole_game_distill, whole_game_fit, whole_game_multislot_fit,
                      whole_game_spatial_stream_fit, whole_game_stream_distill,
                      whole_game_stream_fit)


class WholeGameFitGateTests(unittest.TestCase):
    def make_inputs(self, root):
        releases = []
        release_hashes = []
        for split in ("train", "validation"):
            release = root / f"{split}-release"
            release.mkdir()
            identity = {"selected": [
                {"split": split, "matchup": matchup,
                 "game_id": f"{split}-{matchup}-{index}",
                 "replay_sha256": hashlib.sha256(
                     f"{split}-{matchup}-{index}-replay".encode()).hexdigest()}
                for matchup in MATCHUPS for index in range(2)
            ]}
            identity_path = release / "identity.json"
            identity_path.write_text(json.dumps(identity, sort_keys=True), encoding="utf-8")
            identity_sha = hashlib.sha256(identity_path.read_bytes()).hexdigest()
            (release / "release.json").write_text(json.dumps({
                "complete": True, "training_ready": True,
            }), encoding="utf-8")
            releases.append(release)
            release_hashes.append(identity_sha)

        source_hashes = {
            name: hashlib.sha256((Path(__file__).parents[1] / "training" / name)
                                 .read_bytes()).hexdigest()
            for name in AUDIT_SOURCE_FILES
        }
        def split_report(split):
            cohort_ids = {
                matchup: [f"{split}-{matchup}-{index}" for index in range(2)]
                for matchup in MATCHUPS
            }
            return {
                "cohort_game_ids": cohort_ids,
                "overall": {
                    "complete_windows_within_frame_limit": 10,
                    "event_conditioned_windows": {
                        "action": 4, "no_action": 6, "action_rate_among_complete": 0.4,
                    },
                    "all_complete_windows": {
                        "stop_slots_by_ordinal": {"0": 6},
                        "overflow_commands_beyond_slot_limit": 0,
                        "actor_set_availability": {"available": 4},
                        "actor_ids_absent_from_causal_observation": 0,
                        "entity_target_availability": {"available": 2},
                        "position_labels": {"count": 2, "in_map_bounds": 2},
                        "delay_frames": {"count": 4, "median": 8, "p90": 16},
                    },
                    "censored_final_windows_full_game": 6,
                },
            }
        audit = {
            "schema": "protodd-whole-game-representation-audit-v1",
            "predeclared_limits": PREDECLARED_LIMITS,
            "releases": {
                "train": {"identity_sha256": release_hashes[0],
                          "release_training_ready": True},
                "validation": {"identity_sha256": release_hashes[1],
                               "release_training_ready": True},
            },
            "safety": {
                "final_test_read": False, "optimizer_run": False,
                "train_validation_game_overlap": False,
                "train_validation_replay_overlap": False,
                "validation_labels_modified": False,
            },
            "source_sha256": source_hashes,
            "train": split_report("train"),
            "validation": split_report("validation"),
        }
        audit_path = root / "representation-audit.json"
        audit_path.write_text(json.dumps(audit), encoding="utf-8")
        parity = {
            "schema": PARITY_SCHEMA, "passed": True,
            "max_abs_error": 1e-6, "tolerance": 3e-4,
            "checkpoint_sha256": "a" * 64,
            "package_sha256": "b" * 64,
            "executable_sha256": "c" * 64,
            "cases": [
                {"matchup": matchup, "max_abs_error": 1e-6}
                for matchup in MATCHUPS
            ],
        }
        parity_path = root / "native-parity.json"
        parity_path.write_text(json.dumps(parity), encoding="utf-8")
        return releases[0], releases[1], audit_path, parity_path, audit, parity

    def test_pass_binds_ready_disjoint_releases_metrics_and_native_parity(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit, parity, _, _ = self.make_inputs(Path(directory))
            result = verify_pre_fit_gate(train, validation, audit, parity)
            self.assertTrue(result["passed"])
            self.assertEqual(result["schema"], SCHEMA)
            self.assertEqual(result["native_parity_tolerance"], 3e-4)
            self.assertEqual(len(result["representation_audit_sha256"]), 64)

    def test_not_ready_release_fails_before_fit(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit, parity, _, _ = self.make_inputs(Path(directory))
            receipt = json.loads((train / "release.json").read_text())
            receipt["training_ready"] = False
            (train / "release.json").write_text(json.dumps(receipt), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                verify_pre_fit_gate(train, validation, audit, parity)

    def test_stale_identity_or_safety_failure_rejects_audit(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit_path, parity_path, audit, _ = self.make_inputs(Path(directory))
            audit["releases"]["train"]["identity_sha256"] = "0" * 64
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "identity differs"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

            second_root = Path(directory) / "second"
            second_root.mkdir()
            train, validation, audit_path, parity_path, audit, _ = self.make_inputs(second_root)
            audit["safety"]["train_validation_replay_overlap"] = True
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "train_validation_replay_overlap"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

    def test_release_selection_and_replay_disjointness_are_verified_directly(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit_path, parity_path, audit, _ = self.make_inputs(Path(directory))
            audit["train"]["cohort_game_ids"]["PvP"][0] = "substituted-game-id"
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "audit cohort differs"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

            second_root = Path(directory) / "second"
            second_root.mkdir()
            train, validation, audit_path, parity_path, audit, _ = self.make_inputs(second_root)
            train_identity = json.loads((train / "identity.json").read_text())
            validation_identity_path = validation / "identity.json"
            validation_identity = json.loads(validation_identity_path.read_text())
            validation_identity["selected"][0]["replay_sha256"] = \
                train_identity["selected"][0]["replay_sha256"]
            validation_identity_path.write_text(
                json.dumps(validation_identity, sort_keys=True), encoding="utf-8")
            audit["releases"]["validation"]["identity_sha256"] = hashlib.sha256(
                validation_identity_path.read_bytes()).hexdigest()
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "replay assets overlap"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

    def test_bad_metrics_or_parity_cannot_enter_fit(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit_path, parity_path, audit, parity = self.make_inputs(Path(directory))
            audit["train"]["overall"]["event_conditioned_windows"]["no_action"] = 5
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "metrics are incomplete or inconsistent"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

            audit["train"]["overall"]["event_conditioned_windows"]["no_action"] = 6
            audit_path.write_text(json.dumps(audit), encoding="utf-8")
            parity["passed"] = False
            parity_path.write_text(json.dumps(parity), encoding="utf-8")
            with self.assertRaisesRegex(ValueError, "native export parity did not pass"):
                verify_pre_fit_gate(train, validation, audit_path, parity_path)

    def test_full_fit_entrypoints_gate_before_accelerator_or_optimizer_setup(self):
        with tempfile.TemporaryDirectory() as directory:
            train, validation, audit, parity, _, _ = self.make_inputs(Path(directory))
            receipt = json.loads((train / "release.json").read_text())
            receipt["training_ready"] = False
            (train / "release.json").write_text(json.dumps(receipt), encoding="utf-8")
            args = Namespace(
                release=train, validation_release=validation,
                representation_audit=audit, native_parity_report=parity,
            )
            with patch.object(whole_game_fit.torch.cuda, "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_fit.fit(args)
                available.assert_not_called()
            args.device = "cuda"
            with patch.object(whole_game_multislot_fit.torch.cuda, "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_multislot_fit.fit(args)
                available.assert_not_called()
            with patch.object(whole_game_stream_fit.torch.cuda, "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_stream_fit.fit(args)
                available.assert_not_called()
            with patch.object(whole_game_spatial_stream_fit.torch.cuda,
                              "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_spatial_stream_fit.fit(args)
                available.assert_not_called()
            with patch.object(whole_game_distill.torch.cuda, "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_distill.fit(args)
                available.assert_not_called()
            with patch.object(whole_game_stream_distill.torch.cuda,
                              "is_available") as available:
                with self.assertRaisesRegex(ValueError, "train release is not training_ready"):
                    whole_game_stream_distill.fit(args)
                available.assert_not_called()


if __name__ == "__main__":
    unittest.main()
