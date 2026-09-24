"""Audit entity capacity and pointer-label coverage in completed replay shards."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from .whole_game_features import select_entities
from .whole_game_shards import selected_shards, trajectory_shard


def audit(release, split, limit=512, max_games=None):
    if limit < 1 or (max_games is not None and max_games < 1):
        raise ValueError("positive limits required")
    result = Counter()
    matchups = Counter()
    for record, directory, _ in selected_shards(Path(release), split):
        if max_games is not None and result["games"] >= max_games:
            break
        result["games"] += 1
        matchups[record["matchup"]] += 1
        for row, supervision in trajectory_shard(directory):
            result["observations"] += 1
            counts = Counter(entity["relation"] for entity in row["entities"])
            result["max_own"] = max(result["max_own"], counts[0])
            result["max_total"] = max(result["max_total"], len(row["entities"]))
            result["max_enemy"] = max(result["max_enemy"], counts[1])
            result["own_over_capacity"] += counts[0] > limit
            result["total_over_capacity"] += len(row["entities"]) > limit
            if counts[0] > limit:
                continue
            selected, overflow = select_entities(row, limit)
            selected_ids = {entity["id"] for entity in selected}
            if not supervision["action"]:
                continue
            label = supervision["action"]
            result["action_labels"] += 1
            result["action_labels_with_overflow"] += overflow > 0
            positive = set(label["actor_positive"])
            if not positive.issubset(selected_ids):
                result["actor_labels_unrepresentable"] += 1
            if label["loss_masks"]["target_entity"]:
                result["target_entity_labels"] += 1
                result["target_entity_unrepresentable"] += label["actions"]["target_entity"] not in selected_ids
    return dict(schema="protodd-whole-game-capacity-v1", split=split, entity_limit=limit,
                counts=result, matchup_games=matchups)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("--split", choices=("train", "validation"), default="train")
    parser.add_argument("--entity-limit", type=int, default=512)
    parser.add_argument("--max-games", type=int)
    args = parser.parse_args()
    print(json.dumps(audit(args.release, args.split, args.entity_limit, args.max_games),
                     indent=2, default=dict))


if __name__ == "__main__":
    main()
