"""Measure whether replay position targets coincide with observable anchors.

Runs only on train games omitted from the full fit. This is a coverage study,
not a policy score or promotion artifact.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import select_entities
from .whole_game_shards import selected_shards, trajectory_shard


def probe(release, actor_rank_report, output):
    release, actor_rank_report, output = map(Path, (release, actor_rank_report, output))
    if output.exists():
        raise FileExistsError(output)
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    chosen = parent["train_game_ids"]
    selected = {game_id for ids in chosen.values() for game_id in ids}
    counts = defaultdict(Counter)
    games = Counter()
    for record, directory, _ in selected_shards(
            release, "train", game_ids=selected):
        games[record["matchup"]] += 1
        for sequence in cadence_sequences(trajectory_shard(directory)):
            observation = sequence["observation"]
            entities, _ = select_entities(observation, 512)
            visible = [entity for entity in entities if entity["visible"]]
            groups = {
                "any_visible": visible,
                "owned": [entity for entity in visible if entity["relation"] == 0],
                "enemy": [entity for entity in visible if entity["relation"] == 1],
                "neutral": [entity for entity in visible if entity["relation"] == 2],
            }
            own_by_id = {entity["id"]: entity for entity in entities
                         if entity["relation"] == 0}
            for label in sequence["labels"]:
                if not label["loss_masks"]["target_position"]:
                    continue
                kind = label["actions"]["kind"]
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                x, y = label["actions"]["target_position"]
                target = (x * factor, y * factor)
                bucket = counts[kind]
                bucket["samples"] += 1
                for name, candidates in groups.items():
                    if not candidates:
                        continue
                    distance = min(math.dist(target, entity["position"])
                                   for entity in candidates)
                    for radius in (32, 64, 128, 256):
                        bucket[f"{name}_within_{radius}px"] += distance <= radius
                for actor_id in label["actor_positive"]:
                    actor = own_by_id.get(actor_id)
                    if actor is None:
                        continue
                    position = actor["own_state"]["order_position"]
                    if position[0] < 0 or position[1] < 0:
                        continue
                    distance = math.dist(target, position)
                    for radius in (32, 64, 128, 256):
                        bucket[f"actor_order_within_{radius}px"] += distance <= radius
                    break
    if any(games[matchup] != len(chosen[matchup]) for matchup in chosen):
        raise ValueError("omitted train cohort incomplete")
    overall = Counter()
    for values in counts.values():
        overall.update(values)
    report = dict(
        schema="protodd-whole-game-position-geometry-probe-v1",
        promotion_eligible=False,
        games=dict(games), game_ids=chosen,
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        overall=dict(overall),
        by_kind={kind: dict(values) for kind, values in sorted(counts.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    result = probe(args.release, args.actor_rank_report, args.output)
    print(json.dumps(result["overall"], indent=2))
