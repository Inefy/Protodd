"""Describe first-command combat context on omitted-train games.

This read-only probe checks whether combat labels have visible local evidence
at the preceding cadence observation. It does not evaluate or train a model.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_shards import selected_shards, trajectory_shard


def summarize(rows):
    keys = ("delay_frames", "nearest_enemy_px", "position_distance_px",
            "visible_enemy_count", "frame")
    result = dict(samples=len(rows), actor_types=dict(Counter(
        row["actor_type"] for row in rows).most_common(10)))
    for key in keys:
        values = [row[key] for row in rows if row[key] is not None]
        result[key] = dict(count=len(values), median=statistics.median(values)
                           if values else None)
    result["enemy_within_256px"] = sum(
        row["nearest_enemy_px"] is not None and row["nearest_enemy_px"] <= 256
        for row in rows)
    result["enemy_visible"] = sum(row["visible_enemy_count"] > 0 for row in rows)
    return result


def probe(release, actor_rank_report, output):
    release, actor_rank_report, output = map(Path, (release, actor_rank_report, output))
    if output.exists():
        raise ValueError("existing output")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    train_ids = {game_id for ids in parent["train_game_ids"].values() for game_id in ids}
    dev_ids = set(parent["dev_game_ids"].values())
    groups = {"train": defaultdict(list), "dev": defaultdict(list)}
    games = {"train": Counter(), "dev": Counter()}
    for record, directory, _ in selected_shards(
            release, "train", game_ids=train_ids | dev_ids):
        group = "dev" if record["game_id"] in dev_ids else "train"
        games[group][record["matchup"]] += 1
        for sequence in cadence_sequences(trajectory_shard(directory)):
            if not sequence["labels"] or not sequence["actor_available"][0]:
                continue
            label = sequence["labels"][0]
            row = sequence["observation"]
            entities = {entity["id"]: entity for entity in row["entities"]}
            actors = sorted(set(label["actor_positive"]) & entities.keys())
            if not actors:
                continue
            actor = entities[actors[0]]
            position = actor["position"]
            enemies = [entity for entity in row["entities"]
                       if entity["relation"] == 1 and entity["visible"]]
            distance = lambda point: math.dist(position, point)
            target = label["actions"].get("target_position")
            if target is not None and label["loss_masks"]["target_position"]:
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target_distance = distance([value * factor for value in target])
            else:
                target_distance = None
            groups[group][label["actions"]["kind"]].append(dict(
                delay_frames=label["frame"] - row["frame"],
                nearest_enemy_px=min((distance(enemy["position"]) for enemy in enemies),
                                     default=None),
                position_distance_px=target_distance,
                visible_enemy_count=len(enemies), actor_type=actor["type"],
                frame=row["frame"]))
    if (any(games["train"][matchup] != len(ids)
            for matchup, ids in parent["train_game_ids"].items()) or
        any(games["dev"][matchup] != 1
            for matchup in parent["dev_game_ids"])):
        raise ValueError("combat context cohort incomplete")
    report = dict(
        schema="protodd-whole-game-combat-context-probe-v1",
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        train_game_ids=parent["train_game_ids"], dev_game_ids=parent["dev_game_ids"],
        games={key: dict(value) for key, value in games.items()},
        by_kind={group: {kind: summarize(rows) for kind, rows in sorted(kinds.items())}
                 for group, kinds in groups.items()})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    report = probe(args.release, args.actor_rank_report, args.output)
    print(json.dumps({group: {kind: rows for kind, rows in kinds.items()
                              if kind in ("attack", "attack_move", "right_click")}
                      for group, kinds in report["by_kind"].items()}, indent=2))
