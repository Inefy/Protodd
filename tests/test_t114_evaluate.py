import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from tools.t114_evaluate import (
    _powershell_compact_json,
    _sha256_bytes,
    evaluate_schedule,
    score_result,
    schedule_digest,
    source_inventory_digest,
    stratified_bootstrap,
    verify_schedule,
)


ROOT = Path(__file__).resolve().parents[1]


def schedule_fixture(root):
    schedule_dir = root / "schedule"
    schedule_dir.mkdir()
    schedule = {
        "schedule_id": "portable-evaluation-fixture",
        "status": "frozen-development-schedule-not-yet-executed",
        "development": {"pair_count": 24, "seeds": [123, 456]},
        "arms": {}, "maps": {}, "opponents": {}, "pairs": [],
        "shared_runtime": {
            "runtime_profile": "stock-certification", "runtime_archive_sha256": "a" * 64,
            "bwapi_dll_sha256": "b" * 64, "host_race": "Protoss", "frame_limit": 86400,
            "frame_milliseconds": 0, "save_replay": True, "observer_enabled": False,
            "preserve_learning": False,
        },
    }
    for name in ("reference", "candidate"):
        source = f"// {name} fixture source\n".encode()
        dll = f"{name} fixture DLL bytes".encode()
        files = [{"path": "src/bot.cpp", "sha256": _sha256_bytes(source), "size_bytes": len(source)}]
        source_hash = source_inventory_digest(files)
        build = {"schema": "protodd-build-v1", "source_files": files,
                 "source_snapshot_sha256": source_hash, "dll_sha256": _sha256_bytes(dll),
                 "cmake_options": {"tournament": True}}
        manifest = {"schema": "protodd-tournament-package-v1", "build_manifest": build,
                    "package_dll_sha256": _sha256_bytes(dll)}
        package = schedule_dir / f"{name}.zip"
        with zipfile.ZipFile(package, "w") as archive:
            archive.writestr("manifest.json", json.dumps(manifest))
            archive.writestr("Protodd.dll", dll)
            archive.writestr("source/src/bot.cpp", source)
        schedule["arms"][name] = {"name": name, "package_path": package.name,
            "package_sha256": _sha256_bytes(package.read_bytes()),
            "dll_sha256": _sha256_bytes(dll), "source_snapshot_sha256": source_hash}
    for index in range(4):
        key = f"map{index}"
        data = f"map evidence fixture {index}".encode()
        path = schedule_dir / f"{key}.scx"
        path.write_bytes(data)
        schedule["maps"][key] = {"relative_path": f"maps/{key}.scx",
            "evidence_path": path.name, "sha256": _sha256_bytes(data)}
    for race in ("Protoss", "Terran", "Zerg"):
        name = f"Fixture{race}"
        dll = f"{name} opponent bytes".encode()
        package_hash = _sha256_bytes(f"{name} package".encode())
        package = root / "ladder" / "bots" / name
        (package / "AI").mkdir(parents=True)
        (package / "AI" / f"{name}.dll").write_bytes(dll)
        (package / ".astra-ladder.json").write_text(json.dumps({"sha256": package_hash}), encoding="utf-8")
        schedule["opponents"][race] = {"name": name, "race": race,
            "dll_sha256": _sha256_bytes(dll), "declared_package_sha256": package_hash}
        for map_key in schedule["maps"]:
            for seed in schedule["development"]["seeds"]:
                ordinal = len(schedule["pairs"]) + 1
                pair_id = f"fixture-{race}-{map_key}-{seed}"
                order = ["reference", "candidate"] if ordinal % 2 else ["candidate", "reference"]
                schedule["pairs"].append({"pair_id": pair_id, "ordinal": ordinal,
                    "map": map_key, "matchup": f"Pv{race[0]}", "opponent": race, "seed": seed,
                    "arm_order": order, "runs": [{"arm": arm, "label": pair_id,
                        "runtime_set": f"{pair_id}-{arm}"} for arm in order]})
    schedule["schedule_sha256"] = schedule_digest(schedule)
    return schedule, schedule_dir


class T114EvaluationTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.fixture = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.fixture.cleanup)
        cls.fixture_root = Path(cls.fixture.name)
        cls.schedule, cls.schedule_dir = schedule_fixture(cls.fixture_root)

    def test_frozen_schedule_packages_and_maps_verify(self):
        self.assertEqual([], verify_schedule(self.schedule, self.schedule_dir, self.fixture_root))

    def test_schedule_tampering_is_detected(self):
        altered = dict(self.schedule)
        altered["status"] = "executed"
        self.assertIn("schedule hash mismatch", verify_schedule(altered, self.schedule_dir, self.fixture_root))

    def test_packaged_source_tampering_is_detected(self):
        with tempfile.TemporaryDirectory() as temporary:
            schedule, directory = schedule_fixture(Path(temporary))
            package = directory / schedule["arms"]["candidate"]["package_path"]
            with zipfile.ZipFile(package) as archive:
                entries = {name: archive.read(name) for name in archive.namelist()}
            entries["source/src/bot.cpp"] = b"substituted source"
            with zipfile.ZipFile(package, "w") as archive:
                for name, data in entries.items():
                    archive.writestr(name, data)
            schedule["arms"]["candidate"]["package_sha256"] = _sha256_bytes(package.read_bytes())
            schedule["schedule_sha256"] = schedule_digest(schedule)
            errors = verify_schedule(schedule, directory, Path(temporary))
        self.assertIn("candidate: source file hash differs from package manifest: src/bot.cpp", errors)

    def test_map_evidence_tampering_is_detected(self):
        with tempfile.TemporaryDirectory() as temporary:
            schedule, directory = schedule_fixture(Path(temporary))
            (directory / schedule["maps"]["map0"]["evidence_path"]).write_bytes(b"substituted map")
            errors = verify_schedule(schedule, directory, Path(temporary))
        self.assertIn("map evidence hash mismatch: map0", errors)

    def test_configuration_hash_matches_direct_match_powershell_serialization(self):
        configuration = {"label": "fixture", "game": {"map": "maps/test.scx", "seed": 123},
                         "save": True, "optional": None}
        expected = b'{"label":"fixture","game":{"map":"maps/test.scx","seed":123},"save":true,"optional":null}'
        self.assertEqual(expected, _powershell_compact_json(configuration))

    def test_result_scoring_requires_a_terminal_win_draw_or_loss(self):
        self.assertEqual(1.0, score_result("END,win,2400"))
        self.assertEqual(0.5, score_result("END,draw,2400"))
        self.assertEqual(0.0, score_result("END,loss,2400"))
        self.assertIsNone(score_result("SNAPSHOT,1200,workers=8"))

    def test_stratified_bootstrap_is_reproducible_and_equal_weighted(self):
        values = {"PvP|benzene": [0.0, 1.0], "PvT|destination": [-1.0, 0.0]}
        first = stratified_bootstrap(values, seed=1234, resamples=500)
        second = stratified_bootstrap(values, seed=1234, resamples=500)
        self.assertEqual(first, second)
        self.assertEqual(0.0, first["mean_delta"])
        self.assertEqual(500, first["resamples"])

    def _write_attempt(self, root, pair, run, arm_name, result, tile=(12, 18)):
        arm = self.schedule["arms"][arm_name]
        map_info = self.schedule["maps"][pair["map"]]
        opponent = self.schedule["opponents"][pair["opponent"]]
        shared = self.schedule["shared_runtime"]
        output = root / run["runtime_set"]
        output.mkdir(parents=True, exist_ok=True)
        config = {
            "label": run["label"],
            "runtime_profile": shared["runtime_profile"],
            "runtime_set": run["runtime_set"],
            "runtime_profile_info": {"archive_sha256": shared["runtime_archive_sha256"]},
            "host": {"race": shared["host_race"]},
            "opponent": {"name": opponent["name"], "race": opponent["race"]},
            "game": {
                "map": map_info["relative_path"],
                "map_sha256": map_info["sha256"],
                "frame_limit": shared["frame_limit"],
                "frame_milliseconds": shared["frame_milliseconds"],
                "seed_requested": pair["seed"],
                "save_replay": f"Replays/{run['label']}.rep" if shared["save_replay"] else None,
                "observer_enabled": shared["observer_enabled"],
            },
            "learning": {"preserve": shared["preserve_learning"]},
        }
        preflight = {
            "configuration": config,
            "configuration_sha256": _sha256_bytes(_powershell_compact_json(config)),
        }
        preflight_bytes = json.dumps(preflight, indent=2).encode("utf-8")
        preflight_path = output / f"{run['label']}.preflight.json"
        preflight_path.write_bytes(preflight_bytes)
        replay_path = output / f"{run['label']}.rep"
        replay_bytes = b"verified replay fixture"
        replay_path.write_bytes(replay_bytes)
        record = {
            "label": run["label"],
            "status": "completed",
            "result": result,
            "termination_reason": "completed",
            "runtime_crashes": [],
            "process_cleanup": {"cleanup_complete": True},
            "runtime_profile": shared["runtime_profile"],
            "runtime_profile_archive_sha256": shared["runtime_archive_sha256"],
            "host_bwapi_sha256": shared["bwapi_dll_sha256"],
            "opponent_bwapi_sha256": shared["bwapi_dll_sha256"],
            "bot_sha256": arm["dll_sha256"],
            "opponent": opponent["name"],
            "opponent_race": opponent["race"],
            "opponent_sha256": opponent["dll_sha256"],
            "map_sha256": map_info["sha256"],
            "frame_limit": shared["frame_limit"],
            "frame_limit_reached": False,
            "seed_requested": pair["seed"],
            "seed_observed": pair["seed"],
            "host_race": shared["host_race"],
            "replay_saved": shared["save_replay"],
            "replay_path": str(replay_path),
            "replay_sha256": _sha256_bytes(replay_bytes),
            "replay_bytes": len(replay_bytes),
            "learning_preserved": shared["preserve_learning"],
            "opponent_runtime_learning_reset": True,
            "host_learning_initial": [],
            "opponent_learning_initial": [],
            "preflight_manifest_sha256": _sha256_bytes(preflight_bytes),
        }
        (output / f"{run['label']}.json").write_text(json.dumps(record), encoding="utf-8")
        start_fields = "" if tile is None else f",self_start_tile_x={tile[0]},self_start_tile_y={tile[1]}"
        (output / f"{run['label']}.raw.log").write_text(
            f"MATCH,seed={pair['seed']},map_hash=abcd1234,width=128,height=96{start_fields}\n",
            encoding="utf-8",
        )

    def test_valid_pair_is_scored_and_missing_pairs_remain_incomplete(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000")
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000")
            report = evaluate_schedule(self.schedule, root)
        first = report["pairs"][0]
        self.assertTrue(first["strength_pair_valid"], first["pair_errors"])
        self.assertEqual(-1.0, first["candidate_minus_reference_score"])
        self.assertEqual("incomplete", report["analysis_status"])
        self.assertEqual(1, report["completed_valid_pairs"])
        self.assertEqual(23, sum(pair["reference"]["status"] == "missing" for pair in report["pairs"][1:]))
        self.assertIsNone(report["paired_bootstrap"]["ci95"])

    def test_realized_spawn_mismatch_excludes_pair(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000", tile=(12, 18))
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000", tile=(13, 18))
            report = evaluate_schedule(self.schedule, root)
        first = report["pairs"][0]
        self.assertFalse(first["strength_pair_valid"])
        self.assertIsNone(first["candidate_minus_reference_score"])
        self.assertIn("paired MATCH identity differs for self_start_tile", first["pair_errors"])

    def test_missing_spawn_field_excludes_pair(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000")
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000", tile=None)
            report = evaluate_schedule(self.schedule, root)
        first = report["pairs"][0]
        self.assertFalse(first["strength_pair_valid"])
        self.assertTrue(any("no realized self start tile" in error for error in first["candidate"]["errors"]))

    def test_replay_hash_mismatch_invalidates_attempt(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000")
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000")
            candidate_run = pair["runs"][1]
            replay = root / candidate_run["runtime_set"] / f"{candidate_run['label']}.rep"
            replay.write_bytes(b"substituted replay")
            report = evaluate_schedule(self.schedule, root)
        self.assertFalse(report["pairs"][0]["strength_pair_valid"])
        self.assertTrue(any("replay file hash differs" in error for error in report["pairs"][0]["candidate"]["errors"]))

    def test_opponent_crash_is_retained_without_candidate_failure_attribution(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000")
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000")
            candidate_path = root / pair["runs"][1]["runtime_set"] / f"{pair['runs'][1]['label']}.json"
            record = json.loads(candidate_path.read_text(encoding="utf-8"))
            record.update({"status": "incomplete", "result": None, "termination_reason": "runtime-crash", "runtime_crashes": [{"side": "opponent"}]})
            candidate_path.write_text(json.dumps(record), encoding="utf-8")
            report = evaluate_schedule(self.schedule, root)
        first = report["pairs"][0]
        self.assertFalse(first["strength_pair_valid"])
        self.assertEqual("opponent-attributable", first["candidate"]["failure_attribution"])
        self.assertEqual([], report["candidate_failure_attention"])

    def test_candidate_crash_blocks_progression(self):
        pair = self.schedule["pairs"][0]
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            self._write_attempt(root, pair, pair["runs"][0], "reference", "END,win,3000")
            self._write_attempt(root, pair, pair["runs"][1], "candidate", "END,loss,3000")
            candidate_path = root / pair["runs"][1]["runtime_set"] / f"{pair['runs'][1]['label']}.json"
            record = json.loads(candidate_path.read_text(encoding="utf-8"))
            record.update({"status": "incomplete", "result": None, "termination_reason": "runtime-crash", "runtime_crashes": [{"side": "protodd"}]})
            candidate_path.write_text(json.dumps(record), encoding="utf-8")
            report = evaluate_schedule(self.schedule, root)
        self.assertTrue(any(pair["pair_id"] in item for item in report["candidate_failure_attention"]))


if __name__ == "__main__":
    unittest.main()
