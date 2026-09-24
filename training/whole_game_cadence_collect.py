"""Bounded train-only action samples at deployable replay cadence frames.

The current cadence row and preceding causal observations are inputs. The first
confirmed command in the following 24 frames is a label only. Future command
state never enters the encoded observation or recurrent context.
"""
from __future__ import annotations

from collections import Counter, defaultdict, deque
import hashlib
import random

from .whole_game_fit import MATCHUPS, merge_quality_buckets
from .whole_game_features import static_grid
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def _rng(seed, game_id):
    token = hashlib.sha256(f"{seed}:cadence:{game_id}".encode()).digest()
    return random.Random(int.from_bytes(token[:8], "little"))


def collect_cadence_actions(release, group, quality_index, *, history=8,
                            bucket_limit=64, per_game_bucket_limit=2,
                            high_mmr_threshold=2300, high_mmr_share=0.5,
                            seed=42):
    if (history < 1 or bucket_limit < 1 or per_game_bucket_limit < 1 or
            not 0 <= high_mmr_share <= 1 or set(group) != set(MATCHUPS)):
        raise ValueError("invalid cadence action collection policy")
    selected = {game_id for ids in group.values() for game_id in ids}
    if len(selected) != sum(len(ids) for ids in group.values()):
        raise ValueError("duplicate game in cadence cohort")
    games = Counter()
    event_counts = {matchup: Counter() for matchup in MATCHUPS}
    first_kind_counts = Counter()
    global_seen = Counter()
    tier_buckets = defaultdict(list)
    skipped_unavailable_actor = 0
    action_windows = 0
    for record, directory, _ in selected_shards(release, "train", game_ids=selected):
        matchup, game_id = record["matchup"], record["game_id"]
        if matchup not in MATCHUPS or game_id not in group[matchup]:
            raise ValueError("cadence shard is outside frozen matchup cohort")
        games[matchup] += 1
        terrain = static_grid(load_terrain(directory))
        recent = deque(maxlen=history)
        pending = None
        seen = Counter()
        per_game = defaultdict(list)
        rng = _rng(seed, game_id)
        quality = quality_index["games"][game_id]
        tier = "high" if quality["mmr_claim"] >= high_mmr_threshold else "base"
        meta = dict(game_id=game_id, mmr_claim=quality["mmr_claim"], quality_band=tier,
                    source_observation="cadence", target_window_frames=24)

        def finish():
            nonlocal skipped_unavailable_actor, action_windows
            if pending is None or pending["event"] is None:
                return
            if bool(pending["event"]) != bool(pending["labels"]):
                raise ValueError("cadence event and confirmed actions disagree")
            if not pending["labels"]:
                return
            action_windows += 1
            label = pending["labels"][0]
            first_kind_counts[(matchup, label["actions"]["kind"])] += 1
            own_ids = {entity["id"] for entity in pending["row"]["entities"]
                       if entity["relation"] == 0}
            if (not label["actor_positive"] or
                    not set(label["actor_positive"]) <= own_ids):
                skipped_unavailable_actor += 1
                return
            category = (matchup, label["domain"], label["actions"]["kind"])
            sample = (pending["context"], pending["row"],
                      dict(action=label, update_memory=False), terrain, meta)
            seen[category] += 1
            bucket = per_game[category]
            if len(bucket) < per_game_bucket_limit:
                bucket.append(sample)
            else:
                pick = rng.randrange(seen[category])
                if pick < per_game_bucket_limit:
                    bucket[pick] = sample

        for row, target in trajectory_shard(directory):
            if target["update_memory"]:
                finish()
                if target["event"] is not None:
                    event_counts[matchup].update(windows=1, events=target["event"])
                pending = dict(context=list(recent), row=row, event=target["event"],
                               labels=[])
                recent.append(row)
            elif target["action"] is not None and pending is not None and \
                    row["frame"] < pending["row"]["frame"] + 24:
                pending["labels"].append(target["action"])
        finish()
        for category, samples in per_game.items():
            for sample in samples:
                reservoir = (category, tier)
                global_seen[reservoir] += 1
                bucket = tier_buckets[reservoir]
                if len(bucket) < bucket_limit:
                    bucket.append(sample)
                else:
                    pick = rng.randrange(global_seen[reservoir])
                    if pick < bucket_limit:
                        bucket[pick] = sample
    if any(games[matchup] != len(group[matchup]) for matchup in MATCHUPS):
        raise ValueError("cadence cohort was not fully collected")
    buckets = merge_quality_buckets(tier_buckets, bucket_limit, high_mmr_share)
    if not buckets:
        raise ValueError("no deployable cadence actions in cohort")
    return buckets, dict(games=dict(games), event_counts=event_counts,
                         first_kind_counts=first_kind_counts,
                         action_windows=action_windows,
                         skipped_unavailable_actor=skipped_unavailable_actor,
                         bucket_candidates=sum(global_seen.values()),
                         buckets={"/".join(category): len(samples)
                                  for category, samples in buckets.items()})
