"""Measure how many replay commands a single cadence decision can represent.

Future commands are read only as labels after each causal cadence observation.
The coverage figures are oracle upper bounds, not model accuracy: they let an
oracle choose the most common command packet in each following 24-frame window.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_shards import selected_shards, trajectory_shard


SCHEMA = "protodd-whole-game-window-capacity-v1"
SLOTS = (1, 2, 3, 4, 6, 8)


def _packet(label):
    action = label["actions"]
    return (action["kind"], action["target_mode"],
            action.get("target_entity"), tuple(action.get("target_position") or ()),
            action.get("unit_type"), action.get("technology"),
            action.get("upgrade"), action.get("queue_slot"),
            action.get("order"), action.get("queued"))


def window_capacity(labels):
    """Return optimistic command coverage for one completed cadence window."""
    if not labels:
        return None
    groups = {
        "kind": Counter(label["actions"]["kind"] for label in labels),
        "kind_mode": Counter((label["actions"]["kind"],
                              label["actions"]["target_mode"]) for label in labels),
        "packet": Counter(_packet(label) for label in labels),
    }
    actors = {actor for label in labels for actor in label["actor_positive"]}
    runs = []
    previous = None
    seen_actors = set()
    repeated_actor_commands = 0
    for label in labels:
        packet = _packet(label)
        if packet != previous:
            runs.append(0)
            previous = packet
        runs[-1] += 1
        selected = set(label["actor_positive"])
        repeated_actor_commands += bool(selected & seen_actors)
        seen_actors.update(selected)
    row = dict(commands=len(labels), distinct_kinds=len(groups["kind"]),
               distinct_kind_modes=len(groups["kind_mode"]),
               distinct_packets=len(groups["packet"]), distinct_actors=len(actors),
               ordered_packet_runs=len(runs),
               repeated_actor_commands=repeated_actor_commands,
               domains=sorted({label["domain"] for label in labels}),
               chronological_command_coverage={
                   str(slot): min(slot, len(labels)) for slot in SLOTS},
               chronological_packet_coverage={
                   str(slot): sum(runs[:slot]) for slot in SLOTS})
    for name, counts in groups.items():
        ranked = sorted(counts.values(), reverse=True)
        row[f"oracle_{name}_coverage"] = {
            str(slot): sum(ranked[:slot]) for slot in SLOTS}
    return row


def summarize(rows, cadence_windows):
    commands = sum(row["commands"] for row in rows)
    if not rows or commands < len(rows) or cadence_windows < len(rows):
        raise ValueError("invalid or empty cadence cohort")
    result = dict(cadence_windows=cadence_windows, action_windows=len(rows),
                  replay_commands=commands,
                  multi_command_windows=sum(row["commands"] > 1 for row in rows),
                  multi_kind_windows=sum(row["distinct_kinds"] > 1 for row in rows),
                  multi_domain_windows=sum(len(row["domains"]) > 1 for row in rows),
                  multi_packet_windows=sum(row["distinct_packets"] > 1 for row in rows),
                  repeated_actor_windows=sum(row["repeated_actor_commands"] > 0 for row in rows),
                  repeated_actor_commands=sum(row["repeated_actor_commands"] for row in rows),
                  causal_actor_available_commands=sum(
                      row.get("causal_actor_available_commands", 0) for row in rows),
                  causal_argument_available_commands=sum(
                      row.get("causal_argument_available_commands", 0) for row in rows),
                  command_count_histogram=dict(sorted(Counter(
                      row["commands"] for row in rows).items())),
                  distinct_kind_histogram=dict(sorted(Counter(
                      row["distinct_kinds"] for row in rows).items())),
                  distinct_packet_histogram=dict(sorted(Counter(
                      row["distinct_packets"] for row in rows).items())),
                  ordered_packet_run_histogram=dict(sorted(Counter(
                      row["ordered_packet_runs"] for row in rows).items())),
                  chronological_command_coverage={
                      str(slot): sum(row["chronological_command_coverage"][str(slot)]
                                     for row in rows) for slot in SLOTS},
                  chronological_packet_coverage={
                      str(slot): sum(row["chronological_packet_coverage"][str(slot)]
                                     for row in rows) for slot in SLOTS})
    result["oracle_coverage"] = {
        name: {str(slot): sum(row[f"oracle_{name}_coverage"][str(slot)]
                              for row in rows) for slot in SLOTS}
        for name in ("kind", "kind_mode", "packet")}
    return result


def audit(release, split="validation", max_games_per_matchup=8):
    if split not in ("train", "validation") or max_games_per_matchup < 1:
        raise ValueError("only bounded train/validation cohorts are allowed")
    release = Path(release)
    games, cadence = Counter(), Counter()
    rows = defaultdict(list)
    for record, directory, _ in selected_shards(release, split):
        matchup = record["matchup"]
        if games[matchup] >= max_games_per_matchup:
            continue
        games[matchup] += 1
        for window in cadence_sequences(trajectory_shard(directory)):
            cadence[matchup] += 1
            result = window_capacity(window["labels"])
            if result is not None:
                result["causal_actor_available_commands"] = sum(
                    window["actor_available"])
                result["causal_argument_available_commands"] = sum(
                    actor and target for actor, target in zip(
                        window["actor_available"], window["target_available"]))
                rows[matchup].append(result)
    if not games:
        raise ValueError("no completed replay shards in requested cohort")
    all_rows = [row for matchup_rows in rows.values() for row in matchup_rows]
    return dict(schema=SCHEMA, release=str(release.resolve()), split=split,
                games=dict(games), window_frames=24,
                overall=summarize(all_rows, sum(cadence.values())),
                by_matchup={matchup: summarize(matchup_rows, cadence[matchup])
                            for matchup, matchup_rows in sorted(rows.items())})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--split", choices=("train", "validation"), default="validation")
    parser.add_argument("--max-games-per-matchup", type=int, default=8)
    args = parser.parse_args()
    report = audit(args.release, args.split, args.max_games_per_matchup)
    if args.output.exists():
        raise FileExistsError(args.output)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(report["overall"], indent=2))


if __name__ == "__main__":
    main()
