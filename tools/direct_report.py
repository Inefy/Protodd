#!/usr/bin/env python3
"""Summarize direct-match manifests; shutdown END records are not results."""

from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import unittest

from log_analyzer import analyze, wilson_interval


def diagnose_states(lines: list[str]) -> dict:
    """Extract observed milestones, not exact event timings, from sampled states."""
    result = {"state_samples": 0, "max_probes": 0,
              "first_observed_completed_dragoon_frame": None,
              "first_observed_completed_core_frame": None,
              "first_observed_range_frame": None,
              "first_counterattack_frame": None,
              "gas_assignment_samples": 0, "gas_above_target_samples": 0,
              "last_mined_minerals": None, "last_mined_gas": None}
    for line in lines:
        fields = line.strip().split(",")
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
            if "gasWorkers" in extras and "gasTarget" in extras:
                result["gas_assignment_samples"] += 1
                result["gas_above_target_samples"] += int(int(extras["gasWorkers"]) > int(extras["gasTarget"]))
            for field, key in (("minedMinerals", "last_mined_minerals"), ("minedGas", "last_mined_gas")):
                if field in extras:
                    result[key] = int(extras[field])
        except (ValueError, IndexError):
            continue
    return result


def summarize(records: list[dict]) -> dict:
    completed = [r for r in records if r.get("status") == "completed"
                 and str(r.get("result", "")).startswith(("END,win,", "END,loss,"))]
    wins = sum(str(r["result"]).startswith("END,win,") for r in completed)
    groups = {}
    for record in records:
        components = record.get("opponent_components_sha256")
        bundle = hashlib.sha256(json.dumps(components, sort_keys=True).encode()).hexdigest() \
            if components else "unknown"
        key = (record.get("bot_sha256", "unknown"), record.get("opponent", "unknown"),
               record.get("map", "unknown"), record.get("opponent_sha256", "unknown"),
               record.get("map_sha256", "unknown"), bundle)
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
            "wins": counts["wins"], "losses": counts["losses"],
            "incomplete": counts["incomplete"], "completed": games,
            "win_rate": counts["wins"] / games if games else None,
            "wilson_95": wilson_interval(counts["wins"], games),
        })
    return {
        "attempts": len(records), "completed": len(completed),
        "wins": wins, "losses": len(completed) - wins,
        "incomplete": len(records) - len(completed),
        "win_rate": wins / len(completed) if completed else None,
        "wilson_95": wilson_interval(wins, len(completed)),
        "mixed_binaries": len({key[0] for key in groups}) > 1,
        "segments": segments,
    }


class DirectReportTests(unittest.TestCase):
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

    def test_shutdown_loss_is_incomplete(self):
        report = summarize([{"status": "incomplete", "result": "END,loss,18377"}])
        self.assertEqual((report["completed"], report["losses"], report["incomplete"]), (0, 0, 1))
        self.assertIsNone(report["win_rate"])

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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifests", nargs="*", type=Path)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(DirectReportTests)
        result = unittest.TextTestRunner().run(suite)
        raise SystemExit(0 if result.wasSuccessful() else 1)
    if not args.manifests:
        parser.error("provide direct-match .json manifests")
    records = []
    for path in args.manifests:
        record = json.loads(path.read_text(encoding="utf-8-sig"))
        trace = path.with_suffix(".log")
        if trace.exists():
            lines = trace.read_text(encoding="utf-8-sig").splitlines()
            record["build_diagnostics"] = diagnose_states(lines)
            if record.get("status") == "completed":
                record["telemetry"] = analyze(lines)
        records.append(record)
    print(json.dumps({**summarize(records), "matches": records}, indent=2))


if __name__ == "__main__":
    main()
