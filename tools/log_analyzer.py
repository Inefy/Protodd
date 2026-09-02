#!/usr/bin/env python3
"""Aggregate AstraBot tournament logs using only the Python standard library."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
import unittest
from collections import Counter, defaultdict
from pathlib import Path
from typing import Iterable


def wilson_interval(wins: int, games: int, z: float = 1.959963984540054) -> list[float]:
    if games <= 0:
        return [0.0, 1.0]
    rate = wins / games
    denominator = 1.0 + z * z / games
    center = (rate + z * z / (2.0 * games)) / denominator
    margin = z * math.sqrt(
        rate * (1.0 - rate) / games + z * z / (4.0 * games**2)
    ) / denominator
    return [max(0.0, center - margin), min(1.0, center + margin)]


def analyze(lines: Iterable[str]) -> dict[str, object]:
    games: list[dict[str, object]] = []
    current: dict[str, object] | None = None
    for raw in lines:
        fields = raw.strip().split(",")
        if not fields or not fields[0]:
            continue
        if fields[0] == "START" and len(fields) >= 4:
            current = {
                "map": fields[1],
                "opponent": fields[2],
                "opening": fields[3],
                "states": [],
                "slow_frames": [],
                "errors": 0,
            }
        elif fields[0] == "STATE" and len(fields) >= 11 and current is not None:
            try:
                current["states"].append(
                    {
                        "frame": int(fields[1]),
                        "strategy": fields[2],
                        "posture": fields[3],
                        "enemy_plan": fields[4],
                        "uncertainty": float(fields[5]),
                        "fight_ratio": float(fields[6]),
                        "minerals": int(fields[7]),
                        "gas": int(fields[8]),
                        "supply_used": int(fields[9]),
                        "supply_total": int(fields[10]),
                        "frame_ms": float(fields[11]) if len(fields) >= 12 else 0.0,
                        "runtime_load": fields[12] if len(fields) >= 13 else "unknown",
                    }
                )
            except (TypeError, ValueError):
                continue
        elif fields[0] == "PERF" and len(fields) >= 4 and current is not None:
            try:
                current["slow_frames"].append(
                    {
                        "frame": int(fields[1]),
                        "milliseconds": int(fields[2]) / 1000.0,
                        "load": fields[3],
                    }
                )
            except (TypeError, ValueError):
                continue
        elif fields[0] == "PERF_SUMMARY" and len(fields) >= 8 and current is not None:
            try:
                current["performance"] = {
                    "samples": int(fields[1]),
                    "moving_average_ms": float(fields[2]),
                    "peak_ms": float(fields[3]),
                    "over_42ms": int(fields[4]),
                    "over_55ms": int(fields[5]),
                    "over_1s": int(fields[6]),
                    "over_10s": int(fields[7]),
                }
            except (TypeError, ValueError):
                continue
        elif fields[0] == "ERROR" and current is not None:
            current["errors"] = int(current["errors"]) + 1
        elif fields[0] == "END" and len(fields) >= 3 and current is not None:
            try:
                current["won"] = fields[1] == "win"
                current["frames"] = int(fields[2])
            except ValueError:
                current = None
                continue
            games.append(current)
            current = None

    wins = sum(bool(game["won"]) for game in games)
    snapshots = [state for game in games for state in game["states"]]
    by_opening: dict[str, Counter[str]] = defaultdict(Counter)
    for game in games:
        key = "wins" if game["won"] else "losses"
        by_opening[str(game["opening"])][key] += 1

    def average(field: str) -> float:
        return statistics.fmean(float(state[field]) for state in snapshots) if snapshots else 0.0

    average_minutes = (
        statistics.fmean(float(game["frames"]) for game in games) / 1440.0
        if games
        else 0.0
    )
    performance = [game.get("performance", {}) for game in games]
    slow_frames = [sample for game in games for sample in game["slow_frames"]]

    def performance_sum(field: str) -> int:
        return sum(int(item.get(field, 0)) for item in performance)

    return {
        "games": len(games),
        "wins": wins,
        "losses": len(games) - wins,
        "win_rate": wins / len(games) if games else 0.0,
        "win_rate_wilson_95": wilson_interval(wins, len(games)),
        "average_game_minutes": average_minutes,
        "openings": {name: dict(counts) for name, counts in sorted(by_opening.items())},
        "snapshots": len(snapshots),
        "average_minerals": average("minerals"),
        "average_gas": average("gas"),
        "average_fight_ratio": average("fight_ratio"),
        "average_uncertainty": average("uncertainty"),
        "runtime": {
            "peak_frame_ms": max(
                (float(item.get("peak_ms", 0.0)) for item in performance),
                default=0.0,
            ),
            "slow_frame_records": len(slow_frames),
            "over_42ms": performance_sum("over_42ms"),
            "over_55ms": performance_sum("over_55ms"),
            "over_1s": performance_sum("over_1s"),
            "over_10s": performance_sum("over_10s"),
            "caught_errors": sum(int(game["errors"]) for game in games),
        },
    }


class AnalyzerTests(unittest.TestCase):
    def test_batch_summary(self) -> None:
        sample = [
            "START,Fighting Spirit,Iron,standard\n",
            "STATE,3600,PvT plan,Pressure,FastExpand,0.4,1.3,200,100,50,66\n",
            "PERF,3601,43000,emergency\n",
            "ERROR,3602,recovered exception\n",
            "PERF_SUMMARY,7200,0.8,43.0,1,0,0,0\n",
            "END,win,7200\n",
            "START,Python,Steamhammer,economic\n",
            "END,loss,6000\n",
        ]
        result = analyze(sample)
        self.assertEqual(result["games"], 2)
        self.assertEqual(result["wins"], 1)
        self.assertAlmostEqual(result["win_rate"], 0.5)
        self.assertEqual(result["snapshots"], 1)
        self.assertEqual(result["runtime"]["over_42ms"], 1)
        self.assertEqual(result["runtime"]["caught_errors"], 1)

    def test_empty_interval(self) -> None:
        self.assertEqual(wilson_interval(0, 0), [0.0, 1.0])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="*", type=Path)
    parser.add_argument("--pretty", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        suite = unittest.defaultTestLoader.loadTestsFromTestCase(AnalyzerTests)
        result = unittest.TextTestRunner(verbosity=1).run(suite)
        return 0 if result.wasSuccessful() else 1
    if not args.logs:
        parser.error("provide at least one AstraBot.log file")
    lines = (
        line
        for path in args.logs
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines()
    )
    print(json.dumps(analyze(lines), indent=2 if args.pretty else None, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
