"""Audit paired games played by the compiled whole-game controller.

Operational health and command execution are prerequisites, not evidence of
playing strength. Opponent activity and baseline comparisons remain separate.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import statistics


SCHEMA = "protodd-whole-game-controller-audit-v1"


def _trace(received):
    marker = received / "WholeGame-controller.txt"
    inference = received / "WholeGame-inference.csv"
    log = received / "Protodd.log"
    if not marker.is_file() or marker.read_text(encoding="utf8").strip() != "compiled-control":
        raise ValueError("missing compiled controller marker")
    if not inference.is_file() or not log.is_file():
        raise ValueError("missing controller inference or action trace")
    with inference.open(newline="", encoding="utf8") as stream:
        rows = list(csv.reader(stream))
    if not rows or any(len(row) != 6 for row in rows):
        raise ValueError("missing or malformed inference samples")
    try:
        frames = [int(row[0]) for row in rows]
        times = sorted(float(row[3]) for row in rows)
        if frames != sorted(set(frames)) or any(not math.isfinite(time) or time < 0 for time in times):
            raise ValueError("unordered frames or negative inference time")
    except (ValueError, IndexError) as error:
        raise ValueError("invalid inference samples") from error
    performance = None
    errors = []
    action_totals = {}
    with log.open(encoding="utf8", errors="replace") as stream:
        for line in stream:
            parts = line.strip().split(",")
            if parts[0] == "PERF_SUMMARY":
                performance = parts
            elif parts[0] == "ERROR":
                errors.append(line.strip()[:240])
            elif parts[0] == "ACTION_TOTAL" and len(parts) >= 6:
                try:
                    action_totals[tuple(parts[2:5])] = int(parts[5])
                except ValueError:
                    errors.append("malformed action total")
    model_error = received / "WholeGame-model-error.txt"
    if model_error.exists():
        errors.append(model_error.read_text(encoding="utf8", errors="replace")[:240])
    if performance is None or len(performance) < 4:
        errors.append("missing performance summary")
    attempted = sum(total for (source, stage, _), total in action_totals.items()
                    if source == "whole-game" and stage == "issued")
    accepted = sum(total for (source, stage, outcome), total in action_totals.items()
                   if source == "whole-game" and stage == "issued" and outcome == "accepted")
    return dict(model_ticks=len(rows), first_frame=frames[0], last_frame=frames[-1],
                inference_ms=dict(median=statistics.median(times), maximum=times[-1]),
                max_callback_ms=float(performance[3]) if performance and len(performance) >= 4 else None,
                learned_commands_attempted=attempted, learned_commands_accepted=accepted,
                errors=errors)


def summarize(campaign):
    campaign = Path(campaign)
    manifest = json.loads((campaign / "manifest.json").read_text(encoding="utf8"))
    schedule = [json.loads(line) for line in (campaign / "server/games.jsonl").read_text().splitlines()]
    reports_path = campaign / "server/results.jsonl"
    reports = [json.loads(line) for line in reports_path.read_text().splitlines()] if reports_path.exists() else []
    rows = []
    for game in schedule:
        game_id = game["gameID"]
        pair = [report for report in reports if report["gameID"] == game_id]
        own = [report for report in pair if report["reportingBot"] == manifest["bot"]]
        other = [report for report in pair if report["reportingBot"] != manifest["bot"]]
        if len(own) != 1 or len(other) != 1:
            rows.append(dict(game_id=game_id, complete=False, reason="unpaired tournament report"))
            continue
        received = (campaign / "server/replays/bot-write" / f"game-{game_id}" /
                    manifest["bot"] / "received")
        try:
            trace = _trace(received)
        except ValueError as error:
            rows.append(dict(game_id=game_id, complete=False, reason=str(error)))
            continue
        timeout_frames = sum(int(timer.get("frameCount", 0)) for timer in own[0].get("timers", []))
        normal = (all(report.get("gameEndType") == "NORMAL" and
                      not report.get("crash") and not report.get("gameTimeout") for report in pair)
                  and bool(own[0].get("won")) != bool(other[0].get("won")))
        healthy = (normal and timeout_frames == 0 and not trace["errors"] and
                   trace["max_callback_ms"] is not None and trace["max_callback_ms"] < 42 and
                   trace["learned_commands_accepted"] > 0)
        rows.append(dict(game_id=game_id, complete=True, healthy=healthy,
                         won=own[0].get("won"), timeout_frames=timeout_frames, **trace))
    return dict(schema=SCHEMA, campaign=str(campaign.resolve()),
                scheduled=len(schedule), audited=len(rows),
                controller_operational=all(row.get("healthy") for row in rows),
                strength_validated=False, games=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = summarize(args.campaign)
    encoded = json.dumps(report, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
