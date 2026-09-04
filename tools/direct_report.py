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
        if record.get("status") == "completed" and trace.exists():
            record["telemetry"] = analyze(trace.read_text(encoding="utf-8-sig").splitlines())
        records.append(record)
    print(json.dumps({**summarize(records), "matches": records}, indent=2))


if __name__ == "__main__":
    main()
