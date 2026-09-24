"""Bounded, deterministic replay windows for ordered GPU command training."""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import random

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import static_grid
from .whole_game_fit import MATCHUPS
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def _category(matchup, sequence):
    labels = sequence["labels"]
    if not labels:
        return matchup, "no_action", "stop"
    label = next((label for label in labels
                  if label["actions"]["kind"] != "right_click"), labels[0])
    return matchup, label["domain"], label["actions"]["kind"]


def _offer(bucket, item, seen, limit, rng):
    if len(bucket) < limit:
        bucket.append(item)
    else:
        index = rng.randrange(seen)
        if index < limit:
            bucket[index] = item


def collect_multislot_windows(release, split, group, *, history=8,
                              per_game_category_limit=2,
                              category_limit=64, seed=42):
    if split not in ("train", "validation") or set(group) != set(MATCHUPS):
        raise ValueError("train or validation three-matchup cohort required")
    if history < 1 or per_game_category_limit < 1 or category_limit < 1:
        raise ValueError("positive collection limits required")
    selected = {game_id for ids in group.values() for game_id in ids}
    if len(selected) != sum(len(ids) for ids in group.values()):
        raise ValueError("duplicate game in collection cohort")
    result = defaultdict(list)
    global_seen = Counter()
    games = Counter()
    windows = Counter()
    for record, directory, _ in selected_shards(release, split, game_ids=selected):
        matchup, game_id = record["matchup"], record["game_id"]
        if matchup not in MATCHUPS or game_id not in group[matchup]:
            raise ValueError("replay shard outside frozen cohort")
        games[matchup] += 1
        terrain = static_grid(load_terrain(directory))
        rng = random.Random(int.from_bytes(hashlib.sha256(
            f"{seed}:multislot:{game_id}".encode()).digest()[:8], "little"))
        local = defaultdict(list)
        local_seen = Counter()
        for sequence in cadence_sequences(trajectory_shard(directory), history=history):
            category = _category(matchup, sequence)
            windows[category] += 1
            local_seen[category] += 1
            item = (sequence["context"], sequence["observation"], sequence, terrain,
                    dict(game_id=game_id, matchup=matchup))
            _offer(local[category], item, local_seen[category],
                   per_game_category_limit, rng)
        for category, samples in local.items():
            for sample in samples:
                global_seen[category] += 1
                _offer(result[category], sample, global_seen[category],
                       category_limit, rng)
    if any(games[matchup] != len(group[matchup]) for matchup in MATCHUPS):
        raise ValueError("requested replay cohort is incomplete")
    if not result:
        raise ValueError("no cadence windows in cohort")
    return dict(result), dict(games=dict(games),
                              windows={"/".join(key): value for key, value in windows.items()},
                              retained={"/".join(key): len(value)
                                        for key, value in result.items()})
