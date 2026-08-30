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
                    }
                )
            except (TypeError, ValueError):
                continue
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
    }


class AnalyzerTests(unittest.TestCase):
    def test_batch_summary(self) -> None:
        sample = [
            "START,Fighting Spirit,Iron,standard\n",
            "STATE,3600,PvT plan,Pressure,FastExpand,0.4,1.3,200,100,50,66\n",
            "END,win,7200\n",
            "START,Python,Steamhammer,economic\n",
            "END,loss,6000\n",
        ]
        result = analyze(sample)
        self.assertEqual(result["games"], 2)
        self.assertEqual(result["wins"], 1)
        self.assertAlmostEqual(result["win_rate"], 0.5)
        self.assertEqual(result["snapshots"], 1)

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
