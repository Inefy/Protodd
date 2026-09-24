"""Conservative later-observation evidence for replay command execution.

This is a diagnostic, not a binary completion label. A missing later effect is
unknown: it can reflect latency, cancellation, death or replay-model mismatch.
Only the player's legal future observations are inspected, never hidden truth.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
import math
from pathlib import Path

from .whole_game_shards import selected_shards, trajectory_shard


SCHEMA = "protodd-command-outcome-evidence-v1"
SUPPORTED = {"build", "train", "research", "upgrade", "move", "attack_move", "patrol"}


def observed_progress(label, before, later):
    """Return a positive evidence name, or None when the result is unknown."""
    if (label["frame"] != before["frame"] or
            label["observation_sequence"] != before["sequence"] or
            before["reason"] != "before_command" or later["reason"] != "cadence" or
            before["perspective"] != later["perspective"] or
            not before["frame"] < later["frame"]):
        raise ValueError("outcome requires a later legal cadence for the source command")
    actions = label["actions"]
    kind = actions["kind"]
    if kind not in SUPPORTED:
        return None
    original = {e["id"]: e for e in before["entities"] if e["relation"] == 0}
    current = {e["id"]: e for e in later["entities"] if e["relation"] == 0}
    actors = set(label["actor_positive"])
    if kind == "build" and actions["unit_type"] is not None and actions["target_position"] is not None:
        x, y = actions["target_position"]
        scale = 32 if label["coordinate_space"] == "build_tile" else 1
        x, y = x * scale, y * scale
        for unit_id, entity in current.items():
            if (unit_id not in original and entity["type"] == actions["unit_type"]
                    and math.hypot(entity["position"][0] - x, entity["position"][1] - y) <= 96):
                return "new_matching_structure_near_target"
    elif kind == "train" and actions["unit_type"] is not None:
        unit_type = actions["unit_type"]
        for actor in actors & original.keys() & current.keys():
            old_queue = original[actor]["own_state"]["queue"]
            new_queue = current[actor]["own_state"]["queue"]
            if new_queue.count(unit_type) > old_queue.count(unit_type):
                return "actor_queue_gained_requested_unit"
    elif kind == "research" and actions["technology"] is not None:
        index = actions["technology"]
        if (later["technology_completed"][index] > before["technology_completed"][index]
                or later["technology_in_progress"][index] > before["technology_in_progress"][index]):
            return "requested_technology_progress"
    elif kind == "upgrade" and actions["upgrade"] is not None:
        index = actions["upgrade"]
        if (later["upgrade_levels"][index] > before["upgrade_levels"][index]
                or later["upgrade_in_progress"][index] > before["upgrade_in_progress"][index]):
            return "requested_upgrade_progress"
    elif kind in ("move", "attack_move", "patrol") and actions["target_position"] is not None:
        x, y = actions["target_position"]
        if label["coordinate_space"] == "build_tile":
            x, y = x * 32, y * 32
        for actor in actors & original.keys() & current.keys():
            start = original[actor]["position"]
            end = current[actor]["position"]
            if math.hypot(start[0] - x, start[1] - y) - math.hypot(end[0] - x, end[1] - y) >= 16:
                return "actor_moved_toward_target"
    return None


def outcomes(trajectory, horizon=240):
    """Yield evidence or censoring for accepted commands without future leakage."""
    if horizon < 24 or horizon % 24:
        raise ValueError("horizon must be a positive multiple of 24 frames")
    pending = []
    for row, supervision in trajectory:
        frame = row["frame"]
        label = supervision["action"]
        if label is not None:
            actors = set(label["actor_positive"])
            retained = []
            for previous_label, source in pending:
                if actors.intersection(previous_label["actor_positive"]):
                    yield dict(kind=previous_label["actions"]["kind"], status="superseded",
                               source_frame=source["frame"], target_frame=frame)
                else:
                    retained.append((previous_label, source))
            pending = retained
            if label["actions"]["kind"] in SUPPORTED:
                pending.append((label, row))
            continue
        if not supervision["update_memory"]:
            continue
        retained = []
        for previous_label, source in pending:
            kind = previous_label["actions"]["kind"]
            if frame > source["frame"] + horizon:
                yield dict(kind=kind, status="unobserved_within_window",
                           source_frame=source["frame"], target_frame=frame)
                continue
            if frame <= source["frame"]:
                retained.append((previous_label, source))
                continue
            evidence = observed_progress(previous_label, source, row)
            if evidence:
                yield dict(kind=kind, status="observed_progress", evidence=evidence,
                           source_frame=source["frame"], target_frame=frame)
            elif frame >= source["frame"] + horizon:
                yield dict(kind=kind, status="unobserved_within_window",
                           source_frame=source["frame"], target_frame=frame)
            else:
                retained.append((previous_label, source))
        pending = retained
    for label, source in pending:
        yield dict(kind=label["actions"]["kind"], status="censored_tail",
                   source_frame=source["frame"], target_frame=None)


def audit(release, split="train", max_games=3, horizon=240):
    if max_games < 1:
        raise ValueError("max_games must be positive")
    counts, games = Counter(), 0
    for _, directory, _ in selected_shards(Path(release), split):
        if games >= max_games:
            break
        games += 1
        for row in outcomes(trajectory_shard(directory), horizon):
            counts[f"{row['kind']}/{row['status']}"] += 1
            if "evidence" in row:
                counts[f"evidence/{row['evidence']}"] += 1
    return dict(schema=SCHEMA, split=split, games=games, horizon=horizon,
                counts=dict(sorted(counts.items())),
                limitation="positive observable progress only; missing effects are unknown, not failures")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("--split", choices=("train", "validation"), default="train")
    parser.add_argument("--max-games", type=int, default=3)
    parser.add_argument("--horizon", type=int, default=240)
    args = parser.parse_args()
    print(json.dumps(audit(args.release, args.split, args.max_games, args.horizon), indent=2))


if __name__ == "__main__":
    main()
