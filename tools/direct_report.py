#!/usr/bin/env python3
"""Summarize direct-match manifests; shutdown END records are not results."""

from __future__ import annotations

import argparse
from collections import Counter
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import uuid

from log_analyzer import analyze, wilson_interval
from decision_report import analyze_decisions


def diagnose_states(lines: list[str]) -> dict:
    """Extract observed milestones, not exact event timings, from sampled states."""
    result = {"state_samples": 0, "max_probes": 0,
              "first_observed_completed_dragoon_frame": None,
              "first_observed_completed_core_frame": None,
              "first_observed_range_frame": None,
              "first_counterattack_frame": None,
              "first_enemy_contact_frame": None,
              "first_base_breach_frame": None,
              "first_expansion_frame": None,
              "first_army_zero_frame": None,
              "first_nexus_loss_frame": None,
              "first_attack_frame": None,
              "summary": {}, "events": [],
              "gas_assignment_samples": 0, "gas_above_target_samples": 0,
              "last_mined_minerals": None, "last_mined_gas": None}
    for line in lines:
        fields = line.strip().split(",")
        if fields and fields[0] == "EVENT" and len(fields) >= 4:
            try:
                frame = int(fields[1])
                result["events"].append(
                    {"frame": frame, "kind": fields[2], "value": ",".join(fields[3:])}
                )
                event_milestones = {
                    "enemy-contact": "first_enemy_contact_frame",
                    "base-breach": "first_base_breach_frame",
                    "core-complete": "first_observed_completed_core_frame",
                    "dragoon-complete": "first_observed_completed_dragoon_frame",
                    "range-complete": "first_observed_range_frame",
                    "second-nexus": "first_expansion_frame",
                    "counterattack": "first_counterattack_frame",
                    "attack-posture": "first_attack_frame",
                    "army-zero": "first_army_zero_frame",
                    "nexus-loss": "first_nexus_loss_frame",
                }
                milestone = event_milestones.get(fields[2])
                if milestone is not None and result[milestone] is None:
                    result[milestone] = frame
            except ValueError:
                pass
            continue
        if fields and fields[0] == "SUMMARY":
            values = {}
            for field in fields[1:]:
                if "=" not in field:
                    continue
                key, value = field.split("=", 1)
                try:
                    values[key] = float(value) if "." in value else int(value)
                except ValueError:
                    values[key] = value
            result["summary"] = values
            continue
        if len(fields) < 14 or fields[0] != "STATE":
            continue
        try:
            frame = int(fields[1])
            extras = dict(field.split("=", 1) for field in fields[14:] if "=" in field)
            result["state_samples"] += 1
            result["max_probes"] = max(result["max_probes"], int(extras.get("probes", 0)))
            for name, milestone in (("dragoons", "first_observed_completed_dragoon_frame"),
                                    ("core", "first_observed_completed_core_frame")):
                counts = extras.get(name, "0/0").split("/")
                completed = int(counts[1]) if len(counts) == 2 else 0
                if completed > 0 and result[milestone] is None:
                    result[milestone] = frame
            if result["first_observed_completed_core_frame"] is None and any(
                    structure.startswith("R@") for structure in extras.get("defense", "").split(";")):
                result["first_observed_completed_core_frame"] = frame
            if int(extras.get("range", "0/0").split("/")[0]) > 0 and result["first_observed_range_frame"] is None:
                result["first_observed_range_frame"] = frame
            counterattack = int(extras.get("counterattackFirst", -1))
            if counterattack >= 0 and result["first_counterattack_frame"] is None:
                result["first_counterattack_frame"] = counterattack
            for field, key in (("firstEnemyContact", "first_enemy_contact_frame"),
                               ("firstBaseBreach", "first_base_breach_frame"),
                               ("firstExpansion", "first_expansion_frame"),
                               ("firstArmyZero", "first_army_zero_frame"),
                               ("firstNexusLoss", "first_nexus_loss_frame"),
                               ("firstAttack", "first_attack_frame")):
                if field in extras and int(extras[field]) >= 0 and result[key] is None:
                    result[key] = int(extras[field])
            if "gasWorkers" in extras and "gasTarget" in extras:
                result["gas_assignment_samples"] += 1
                result["gas_above_target_samples"] += int(int(extras["gasWorkers"]) > int(extras["gasTarget"]))
            for field, key in (("minedMinerals", "last_mined_minerals"), ("minedGas", "last_mined_gas")):
                if field in extras:
                    result[key] = int(extras[field])
        except (ValueError, IndexError):
            continue
    summary = result["summary"]
    for field, key in (("firstEnemyContact", "first_enemy_contact_frame"),
                       ("firstBaseBreach", "first_base_breach_frame"),
                       ("firstCore", "first_observed_completed_core_frame"),
                       ("firstDragoon", "first_observed_completed_dragoon_frame"),
                       ("firstRange", "first_observed_range_frame"),
                       ("firstExpansion", "first_expansion_frame"),
                       ("firstCounterattack", "first_counterattack_frame"),
                       ("firstArmyZero", "first_army_zero_frame"),
                       ("firstNexusLoss", "first_nexus_loss_frame"),
                       ("firstAttack", "first_attack_frame")):
        if key in result and result[key] is None and field in summary and int(summary[field]) >= 0:
            result[key] = int(summary[field])
    for field, key in (("maxProbes", "max_probes"),):
        if field in summary:
            result[key] = max(int(result[key]), int(summary[field]))
    result["decision_diagnostics"] = analyze_decisions(lines)
    return result


def parse_match_identity(lines: list[str]) -> dict | None:
    """Read the single MATCH header, including the realized Protodd start tile."""
    rows = [line.strip().split(",") for line in lines if line.startswith("MATCH,")]
    if not rows:
        return None
    if len(rows) != 1:
        raise ValueError("trace must contain exactly one MATCH header")
    fields = {}
    for item in rows[0][1:]:
        if "=" not in item:
            continue
        key, value = item.split("=", 1)
        if key in fields:
            raise ValueError(f"duplicate MATCH field: {key}")
        fields[key] = value
    try:
        identity = {
            "seed": int(fields["seed"]),
            "map_hash": fields["map_hash"],
            "width": int(fields["width"]),
            "height": int(fields["height"]),
        }
    except (KeyError, ValueError) as error:
        raise ValueError("MATCH header lacks a valid seed, map hash, or dimensions") from error
    start_x, start_y = fields.get("self_start_tile_x"), fields.get("self_start_tile_y")
    if (start_x is None) != (start_y is None):
        raise ValueError("MATCH header has incomplete self start-tile coordinates")
    identity["self_start_tile"] = None if start_x is None else [int(start_x), int(start_y)]
    return identity


def summarize(records: list[dict]) -> dict:
    completed = [r for r in records if r.get("status") == "completed"
                 and str(r.get("result", "")).startswith(("END,win,", "END,loss,"))]
    completed_ids = {id(record) for record in completed}
    wins = sum(str(r["result"]).startswith("END,win,") for r in completed)
    incomplete_reasons = Counter(
        str(record.get("termination_reason", "unknown"))
        for record in records if id(record) not in completed_ids
    )
    groups = {}
    for record in records:
        components = record.get("opponent_components_sha256")
        bundle = hashlib.sha256(json.dumps(components, sort_keys=True).encode()).hexdigest() \
            if components else "unknown"
        conditions = json.dumps({field: record.get(field, "unknown") for field in (
            "opponent_type", "opponent_opening_requested", "opponent_strategy_requested",
            "opponent_runtime_configuration_sha256",
            "opponent_runtime_strategy_configuration_sha256",
            "opponent_runtime_learning_reset", "learning_preserved")}, sort_keys=True)
        key = (record.get("bot_sha256", "unknown"), record.get("opponent", "unknown"),
               record.get("map", "unknown"), record.get("opponent_sha256", "unknown"),
               record.get("map_sha256", "unknown"), bundle, conditions)
        counts = groups.setdefault(key, Counter())
        if record in completed:
            counts["wins" if str(record["result"]).startswith("END,win,") else "losses"] += 1
        else:
            counts["incomplete"] += 1
    segments = []
    for key, counts in sorted(groups.items()):
        games = counts["wins"] + counts["losses"]
        segments.append({
            "bot_sha256": key[0], "opponent": key[1], "map": key[2],
            "opponent_sha256": key[3], "map_sha256": key[4],
            "opponent_bundle_sha256": key[5],
            "test_conditions": json.loads(key[6]),
            "wins": counts["wins"], "losses": counts["losses"],
            "incomplete": counts["incomplete"], "completed": games,
            "win_rate": counts["wins"] / games if games else None,
            "wilson_95": wilson_interval(counts["wins"], games),
        })
    return {
        "attempts": len(records), "completed": len(completed),
        "wins": wins, "losses": len(completed) - wins,
        "incomplete": len(records) - len(completed),
        "incomplete_reasons": dict(sorted(incomplete_reasons.items())),
        "win_rate": wins / len(completed) if completed else None,
        "wilson_95": wilson_interval(wins, len(completed)),
        "mixed_binaries": len({key[0] for key in groups}) > 1,
        "segments": segments,
    }


def ingest_manifests(paths: list[Path]) -> tuple[list[dict], int]:
    """Load immutable match evidence once; reject changed copies of one run."""
    ordered_paths = sorted((Path(path).resolve(strict=True) for path in paths), key=str)
    records: list[dict] = []
    seen: dict[str, tuple[str, str | None]] = {}
    duplicates = 0
    for path in ordered_paths:
        manifest_bytes = path.read_bytes()
        manifest_sha256 = hashlib.sha256(manifest_bytes).hexdigest()
        record = json.loads(manifest_bytes.decode("utf-8-sig"))
        if not isinstance(record, dict):
            raise ValueError(f"match manifest must be a JSON object: {path}")
        preflight_sha256 = record.get("preflight_manifest_sha256")
        if preflight_sha256:
            identity = f"preflight:{str(preflight_sha256).lower()}"
        elif record.get("label") and record.get("started_utc"):
            identity = f"legacy:{record['label']}:{record['started_utc']}"
        else:
            identity = f"manifest:{manifest_sha256}"

        trace_path = path.with_suffix(".log")
        trace_bytes = trace_path.read_bytes() if trace_path.exists() else None
        trace_sha256 = hashlib.sha256(trace_bytes).hexdigest() if trace_bytes is not None else None
        fingerprint = (manifest_sha256, trace_sha256)
        previous = seen.get(identity)
        if previous is not None:
            if previous != fingerprint:
                raise ValueError(f"conflicting evidence for match identity {identity}")
            duplicates += 1
            continue

        record["source_manifest_path"] = str(path)
        record["source_manifest_sha256"] = manifest_sha256
        record["ingestion_identity"] = identity
        if trace_bytes is not None:
            lines = trace_bytes.decode("utf-8-sig").splitlines()
            record["source_trace_sha256"] = trace_sha256
            record["match_identity"] = parse_match_identity(lines)
            record["build_diagnostics"] = diagnose_states(lines)
            if record.get("status") == "completed":
                record["telemetry"] = analyze(lines)
        records.append(record)
        seen[identity] = fingerprint
    return records, duplicates


@contextmanager
def _exclusive_index_lock(path: Path):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a+b") as stream:
        if os.name == "nt":
            import msvcrt

            stream.seek(0, os.SEEK_END)
            if stream.tell() == 0:
                stream.write(b"\0")
                stream.flush()
            stream.seek(0)
            try:
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            except OSError as error:
                raise RuntimeError(f"campaign ingestion index is already locked: {path}") from error
            try:
                yield
            finally:
                stream.seek(0)
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl

            fcntl.flock(stream.fileno(), fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def _atomic_replace_json(path: Path, value: dict) -> None:
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    encoded = (json.dumps(value, indent=2, sort_keys=True) + "\n").encode("utf-8")
    try:
        with temporary.open("xb") as stream:
            stream.write(encoded)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        try:
            temporary.unlink()
        except FileNotFoundError:
            pass


def update_ingestion_index(path: Path, incoming: list[dict]) -> tuple[list[dict], int, int]:
    """Atomically add immutable runs to a cross-invocation campaign index."""
    path = Path(path).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    lock_path = path.with_name(path.name + ".lock")
    with _exclusive_index_lock(lock_path):
        if path.exists():
            index = json.loads(path.read_text(encoding="utf-8"))
            if index.get("schema") != "protodd-direct-ingestion-v1" or not isinstance(index.get("matches"), list):
                raise ValueError(f"unsupported direct-report ingestion index: {path}")
            records = index["matches"]
        else:
            records = []

        by_identity: dict[str, dict] = {}
        for record in records:
            identity = str(record.get("ingestion_identity", ""))
            if not identity or identity in by_identity:
                raise ValueError(f"ingestion index has a missing or duplicate identity: {path}")
            by_identity[identity] = record

        newly_ingested = 0
        already_ingested = 0
        changed = False
        for record in incoming:
            identity = str(record.get("ingestion_identity", ""))
            if not identity:
                raise ValueError("incoming match evidence has no stable ingestion identity")
            previous = by_identity.get(identity)
            if previous is not None:
                old_fingerprint = (previous.get("source_manifest_sha256"), previous.get("source_trace_sha256"))
                new_fingerprint = (record.get("source_manifest_sha256"), record.get("source_trace_sha256"))
                if old_fingerprint != new_fingerprint:
                    raise ValueError(f"conflicting evidence for already-ingested match identity {identity}")
                already_ingested += 1
                continue
            by_identity[identity] = record
            newly_ingested += 1
            changed = True

        if changed or not path.exists():
            ordered_records = [by_identity[key] for key in sorted(by_identity)]
            _atomic_replace_json(path, {
                "schema": "protodd-direct-ingestion-v1",
                "matches": ordered_records,
            })
        else:
            ordered_records = [by_identity[key] for key in sorted(by_identity)]
        return ordered_records, newly_ingested, already_ingested


class DirectReportTests(unittest.TestCase):
    def test_match_identity_includes_realized_start_tile(self):
        identity = parse_match_identity([
            "MATCH,seed=20261101,map_hash=abc123,width=4096,height=3072,"
            "self_start_tile_x=14,self_start_tile_y=22"
        ])
        self.assertEqual(identity, {
            "seed": 20261101, "map_hash": "abc123", "width": 4096,
            "height": 3072, "self_start_tile": [14, 22],
        })

    def test_match_identity_rejects_incomplete_start_tile(self):
        with self.assertRaisesRegex(ValueError, "incomplete self start-tile"):
            parse_match_identity([
                "MATCH,seed=1,map_hash=abc,width=4096,height=3072,self_start_tile_x=14"
            ])

    def test_legacy_match_identity_marks_spawn_unavailable(self):
        identity = parse_match_identity([
            "MATCH,seed=1,map_hash=abc,width=4096,height=3072"
        ])
        self.assertIsNone(identity["self_start_tile"])

    def test_runtime_openings_and_learning_are_separate_conditions(self):
        base = {"status": "completed", "result": "END,loss,12000",
                "bot_sha256": "same", "opponent": "BananaBrain",
                "opponent_runtime_learning_reset": True,
                "opponent_opening_requested": "PvP_nzcore"}
        report = summarize([base, {**base, "opponent_opening_requested": "PvP_3gaterobo"},
                            {**base, "opponent_runtime_learning_reset": False}])
        self.assertEqual(len(report["segments"]), 3)
        self.assertTrue(all(segment["completed"] == 1 for segment in report["segments"]))

    def test_milestones_require_completed_units(self):
        prefix = "STATE,{frame},Plan,Hold,Unknown,1,1,100,50,30,50,0.1,normal,idle,"
        states = [prefix.format(frame=3600) + "probes=12,dragoons=1/0,core=1/1,range=0/1",
                  prefix.format(frame=3960) + "probes=13,dragoons=1/1,range=1/0,counterattackFirst=3652"]
        result = diagnose_states(states)
        self.assertEqual(result["first_observed_completed_core_frame"], 3600)
        self.assertEqual(result["first_observed_completed_dragoon_frame"], 3960)
        self.assertEqual(result["first_observed_range_frame"], 3960)
        self.assertEqual(result["max_probes"], 13)
        self.assertEqual(result["first_counterattack_frame"], 3652)
        self.assertIsNone(result["last_mined_minerals"])
        self.assertEqual(result["gas_assignment_samples"], 0)

    def test_mining_observation_and_legacy_core(self):
        state = ("STATE,7200,Plan,Hold,Unknown,1,1,100,50,30,50,0.1,normal,idle,"
                 "defense=N@256x256;R@400x256,gasWorkers=4,gasTarget=3,"
                 "minedMinerals=2400,minedGas=300")
        result = diagnose_states([state])
        self.assertEqual(result["first_observed_completed_core_frame"], 7200)
        self.assertEqual(result["last_mined_minerals"], 2400)
        self.assertEqual(result["last_mined_gas"], 300)
        self.assertEqual(result["gas_above_target_samples"], 1)

    def test_summary_and_events_are_retained(self):
        result = diagnose_states([
            "EVENT,4008,enemy-contact,4008",
            "SUMMARY,won=0,frames=5000,maxProbes=19,firstArmyZero=4200",
        ])
        self.assertEqual(result["events"][0]["kind"], "enemy-contact")
        self.assertEqual(result["first_enemy_contact_frame"], 4008)
        self.assertEqual(result["first_army_zero_frame"], 4200)
        self.assertEqual(result["max_probes"], 19)

    def test_shutdown_loss_is_incomplete(self):
        report = summarize([{"status": "incomplete", "result": "END,loss,18377",
                             "termination_reason": "frame-limit"}])
        self.assertEqual((report["completed"], report["losses"], report["incomplete"]), (0, 0, 1))
        self.assertIsNone(report["win_rate"])
        self.assertEqual(report["incomplete_reasons"], {"frame-limit": 1})

    def test_split_by_binary_and_keep_failures(self):
        records = [{"status": "completed", "result": "END,win,20000", "bot_sha256": "a"},
                   {"status": "completed", "result": "END,loss,30000", "bot_sha256": "b"},
                   {"status": "incomplete", "result": None, "bot_sha256": "b"}]
        report = summarize(records)
        self.assertEqual((report["wins"], report["losses"], report["incomplete"]), (1, 1, 1))
        self.assertEqual(len(report["segments"]), 2)
        self.assertEqual(report["win_rate"], 0.5)
        self.assertTrue(report["mixed_binaries"])
        self.assertEqual(report["segments"][0]["win_rate"], 1.0)
        self.assertEqual(report["segments"][1]["win_rate"], 0.0)
        self.assertEqual(report["segments"][1]["completed"], 1)
        self.assertEqual(report["segments"][0]["wilson_95"], wilson_interval(1, 1))

    def test_missing_terminal_result_cannot_count(self):
        self.assertEqual(summarize([{"status": "completed", "result": None}])["completed"], 0)

    def test_opponent_configuration_changes_split_results(self):
        first = {"status": "completed", "result": "END,win,20000",
                 "opponent_components_sha256": {"Configuration.txt": "one"}}
        second = {**first, "opponent_components_sha256": {"Configuration.txt": "two"}}
        self.assertEqual(len(summarize([first, second])["segments"]), 2)

    def test_runtime_strategy_changes_split_results(self):
        first = {"status": "completed", "result": "END,win,20000",
                 "opponent_strategy_requested": "Terran_TankPush",
                 "opponent_runtime_strategy_configuration_sha256": "one"}
        second = {**first, "opponent_strategy_requested": "Terran_VultureRush",
                  "opponent_runtime_strategy_configuration_sha256": "two"}
        self.assertEqual(len(summarize([first, second])["segments"]), 2)

    def test_manifest_ingestion_deduplicates_identical_copies(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = {"label": "paired-001", "preflight_manifest_sha256": "a" * 64,
                      "started_utc": "2026-10-07T00:00:00Z", "status": "completed",
                      "result": "END,win,20000"}
            encoded = json.dumps(record, sort_keys=True).encode("utf-8")
            first = root / "first.json"
            copy = root / "copy.json"
            first.write_bytes(encoded)
            copy.write_bytes(encoded)

            ingested, duplicates = ingest_manifests([copy, first, first])

            self.assertEqual((len(ingested), duplicates), (1, 2))
            self.assertEqual(summarize(ingested)["completed"], 1)
            self.assertEqual(ingested[0]["source_manifest_sha256"], hashlib.sha256(encoded).hexdigest())

    def test_manifest_ingestion_rejects_conflicting_copy(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.json"
            changed = root / "changed.json"
            common = {"label": "paired-002", "preflight_manifest_sha256": "b" * 64,
                      "started_utc": "2026-10-07T00:01:00Z", "status": "completed"}
            first.write_text(json.dumps({**common, "result": "END,win,20000"}), encoding="utf-8")
            changed.write_text(json.dumps({**common, "result": "END,loss,20000"}), encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "conflicting evidence"):
                ingest_manifests([first, changed])

    def test_manifest_ingestion_rejects_changed_trace(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = {"label": "paired-003", "preflight_manifest_sha256": "c" * 64,
                      "started_utc": "2026-10-07T00:02:00Z", "status": "incomplete"}
            encoded = json.dumps(record, sort_keys=True).encode("utf-8")
            first = root / "first.json"
            changed = root / "changed.json"
            first.write_bytes(encoded)
            changed.write_bytes(encoded)
            first.with_suffix(".log").write_text("TRACE,first\n", encoding="utf-8")
            changed.with_suffix(".log").write_text("TRACE,changed\n", encoding="utf-8")

            with self.assertRaisesRegex(ValueError, "conflicting evidence"):
                ingest_manifests([first, changed])

    def test_cli_counts_identical_manifest_copies_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            record = {"label": "cli-duplicate", "preflight_manifest_sha256": "d" * 64,
                      "status": "completed", "result": "END,win,1200"}
            encoded = json.dumps(record, sort_keys=True)
            first = root / "first.json"
            copy = root / "copy.json"
            index = root / "campaign-index.json"
            first.write_text(encoded, encoding="utf-8")
            copy.write_text(encoded, encoding="utf-8")
            command = [sys.executable, str(Path(__file__).resolve()),
                       "--ingestion-index", str(index), str(first), str(copy)]
            process = subprocess.run(
                command,
                capture_output=True, text=True, check=False,
            )

            self.assertEqual(process.returncode, 0, process.stderr)
            report = json.loads(process.stdout)
            self.assertEqual((report["attempts"], report["completed"]), (1, 1))
            self.assertEqual(report["ingestion"]["duplicates_skipped"], 1)
            self.assertEqual(report["ingestion"]["newly_ingested"], 1)
            original_index = index.read_bytes()

            repeated = subprocess.run(command, capture_output=True, text=True, check=False)
            self.assertEqual(repeated.returncode, 0, repeated.stderr)
            repeated_report = json.loads(repeated.stdout)
            self.assertEqual(repeated_report["attempts"], 1)
            self.assertEqual(repeated_report["ingestion"]["already_ingested"], 1)
            self.assertEqual(repeated_report["ingestion"]["newly_ingested"], 0)
            self.assertEqual(index.read_bytes(), original_index)

            conflicting = root / "conflicting.json"
            conflicting.write_text(json.dumps({**record, "result": "END,loss,1200"}), encoding="utf-8")
            conflict = subprocess.run(
                [sys.executable, str(Path(__file__).resolve()),
                 "--ingestion-index", str(index), str(conflicting)],
                capture_output=True, text=True, check=False,
            )
            self.assertNotEqual(conflict.returncode, 0)
            self.assertIn("conflicting evidence", conflict.stderr)
            self.assertEqual(index.read_bytes(), original_index)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="*", type=Path)
    parser.add_argument("--ingestion-index", type=Path,
                        help="atomically retain campaign evidence across repeated report invocations")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(DirectReportTests)
        result = unittest.TextTestRunner().run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if not args.manifests:
        parser.error("provide direct-match .json manifests")
    incoming, batch_duplicates = ingest_manifests(args.manifests)
    if args.ingestion_index:
        records, newly_ingested, already_ingested = update_ingestion_index(args.ingestion_index, incoming)
        index_path = str(args.ingestion_index.resolve())
    else:
        records = incoming
        newly_ingested = len(incoming)
        already_ingested = 0
        index_path = None
    print(json.dumps({
        **summarize(records),
        "ingestion": {
            "input_manifests": len(args.manifests),
            "unique_input_manifests": len(incoming),
            "duplicates_skipped": batch_duplicates + already_ingested,
            "newly_ingested": newly_ingested,
            "already_ingested": already_ingested,
            "total_indexed_manifests": len(records),
            "index_path": index_path,
        },
        "matches": records,
    }, indent=2))


if __name__ == "__main__":
    main()
