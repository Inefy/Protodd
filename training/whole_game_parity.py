"""Audit legal live BWAPI observations against the replay of the same game.

Entity tokens are local to each adapter, so compare their observable values as
multisets. This audit reports mismatch rates and never grants training readiness
on its own; command timing and hidden-state adversarial tests are separate gates.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from .whole_game_pilot import SCHEMA


SCALARS = ("minerals", "gas", "supply_used", "supply_total", "technology_completed",
           "technology_in_progress", "upgrade_levels", "upgrade_in_progress", "vision")


def legal(row, seen):
    if row.get("schema") != SCHEMA or row.get("reason") != "cadence":
        raise ValueError("invalid live observation schema or sampling reason")
    ids = [e["id"] for e in row["entities"]]
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate entity token")
    seen.update(ids)
    for entity in row["entities"]:
        if not 0 <= entity["first_seen"] <= entity["last_seen"] <= row["frame"]:
            raise ValueError("noncausal entity history")
        if entity["visible"] and entity["last_seen"] != row["frame"]:
            raise ValueError("current sighting has wrong frame")
        if entity["relation"] == 0:
            state = entity["own_state"]
            if not isinstance(state, dict) or (state["order_target"] != -1 and state["order_target"] not in seen):
                raise ValueError("own order target is not a legal known entity")
            if not set(state["cargo"]) <= seen:
                raise ValueError("cargo contains unobserved token")
        elif entity["own_state"] is not None:
            raise ValueError("enemy or neutral private state exposed")


def signature(e):
    return (e["relation"], e["type"], *e["position"], e["hp"], e["shields"],
            e["completed"], e["visible"], e["own_state"]["energy"] if e["relation"] == 0 else None,
            e["own_state"]["order"] if e["relation"] == 0 else None)


def compare(live_file, replay_file, live_terrain, replay_terrain):
    live_file, replay_file = Path(live_file), Path(replay_file)
    with live_file.open(encoding="utf-8") as stream:
        live_rows = [json.loads(line) for line in stream if line.strip()]
    with replay_file.open(encoding="utf-8") as stream:
        replay_rows = {row["frame"]: row for line in stream
                       if (row := json.loads(line))["reason"] == "cadence"}
    if not live_rows or not replay_rows:
        raise ValueError("empty live or replay cadence")
    seen = set()
    for row in live_rows:
        legal(row, seen)
    live_frames = [r["frame"] for r in live_rows]
    if live_frames != sorted(set(live_frames)):
        raise ValueError("live cadence reordered or duplicated")
    counts = Counter()
    field_mismatches = Counter()
    examples = []
    for row in live_rows:
        if row["frame"] not in replay_rows:
            continue
        other = replay_rows[row["frame"]]
        counts["frames"] += 1
        for key in SCALARS:
            counts[f"{key}_match"] += row[key] == other[key]
        live_entities = Counter(signature(e) for e in row["entities"])
        replay_entities = Counter(signature(e) for e in other["entities"])
        intersection = sum((live_entities & replay_entities).values())
        counts["live_entities"] += sum(live_entities.values())
        counts["replay_entities"] += sum(replay_entities.values())
        counts["matching_entities"] += intersection
        def coarse_index(entities):
            index = {}
            for e in entities:
                index.setdefault((e["relation"], e["type"], tuple(e["position"])), []).append(e)
            return index
        left, right = coarse_index(row["entities"]), coarse_index(other["entities"])
        for key in left.keys() & right.keys():
            if len(left[key]) != 1 or len(right[key]) != 1:
                continue  # duplicate positions cannot be paired unambiguously
            counts["paired_entities"] += 1
            a, b = left[key][0], right[key][0]
            for field in ("hp", "shields", "completed", "visible"):
                field_mismatches[field] += a[field] != b[field]
            if a["relation"] == 0:
                for field in ("energy", "ground_cooldown", "air_cooldown", "order", "queue", "cargo"):
                    field_mismatches[field] += a["own_state"][field] != b["own_state"][field]
        if row["minerals"] != other["minerals"] or row["gas"] != other["gas"] or intersection != len(row["entities"]):
            if len(examples) < 10:
                examples.append(dict(frame=row["frame"], live_minerals=row["minerals"], replay_minerals=other["minerals"],
                                     live_gas=row["gas"], replay_gas=other["gas"], live_entities=len(row["entities"]),
                                     matching_entities=intersection))
    if counts["frames"] < 2:
        raise ValueError("replay and live game have no useful aligned cadence")
    live_terrain = json.loads(Path(live_terrain).read_text())
    replay_terrain = json.loads(Path(replay_terrain).read_text())
    if (live_terrain["schema"] != replay_terrain["schema"] or
            live_terrain["width_walktiles"] != replay_terrain["width_walktiles"] or
            live_terrain["height_walktiles"] != replay_terrain["height_walktiles"]):
        raise ValueError("incompatible terrain or map dimensions")
    a, b = live_terrain["walkability"], replay_terrain["walkability"]
    if len(a) != len(b):
        raise ValueError("walkability grid length differs")
    terrain_matches = sum(x == y for x, y in zip(a, b))
    return dict(schema=SCHEMA, frames_live=len(live_rows), frames_replay=len(replay_rows),
                aligned_frames=counts["frames"], scalar_matches={k: counts[f"{k}_match"] for k in SCALARS},
                live_entities=counts["live_entities"], replay_entities=counts["replay_entities"],
                matching_entities=counts["matching_entities"], paired_entities=counts["paired_entities"],
                field_mismatches=dict(field_mismatches), walktiles=len(a), walkability_matches=terrain_matches,
                height_compared=False, examples=examples, training_ready=False)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    for name in ("live_file", "replay_file", "live_terrain", "replay_terrain"):
        p.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    args = p.parse_args()
    report = compare(args.live_file, args.replay_file, args.live_terrain, args.replay_terrain)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
