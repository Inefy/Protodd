"""GPU action fit with owned-actor-conditioned command and argument heads.

Uses the exact frozen cohort. Action labels come from deployable cadence frames,
with mixed rare/natural first-command sampling; timing uses each training
group's natural event frequency rather than a 50/50 event mix.
"""
from __future__ import annotations

from . import whole_game_stream_fit_mixed as mixed  # Installs structured objective.
from . import whole_game_stream_fit as stream
from .whole_game_cadence_collect import collect_cadence_actions
from .whole_game_conditional_model import (ConditionalWholeGameModel,
                                           initialize_from_baseline)
from .whole_game_event_sampling import group_seed, natural_event_schedule


stream.SCHEMA = "protodd-whole-game-stream-fit-conditional-v1"
stream.SOURCE_FILES = (*stream.SOURCE_FILES,
                       "whole_game_event_prior.py", "whole_game_event_sampling.py",
                       "whole_game_cadence_collect.py", "whole_game_sequences.py",
                       "whole_game_shards.py",
                       "whole_game_conditional_model.py",
                       "whole_game_stream_fit_conditional.py")
stream.WholeGameModel = ConditionalWholeGameModel
stream.initialize_model = initialize_from_baseline
_mixed_collect_group = stream._collect_group
_mixed_category_keys = stream._category_keys
_event_counts = None
_event_seed = None
_event_slots = None


def _collect_group(args, group, quality, seed):
    global _event_counts, _event_seed, _event_slots
    buckets, info = _mixed_collect_group(args, group, quality, seed)
    for category in list(buckets):
        if category[1] not in ("event", "forecast"):
            del buckets[category]
    cadence, cadence_info = collect_cadence_actions(
        args.release, group, quality, history=args.history,
        bucket_limit=args.bucket_limit, per_game_bucket_limit=args.per_game_bucket_limit,
        high_mmr_threshold=args.high_mmr_threshold,
        high_mmr_share=args.high_mmr_share, seed=seed)
    if set(cadence) & set(buckets):
        raise ValueError("cadence actions collided with timing or future buckets")
    buckets.update(cadence)
    mixed._group_counts = cadence_info["first_kind_counts"]
    info["cadence_action_windows"] = cadence_info["action_windows"]
    info["cadence_skipped_unavailable_actor"] = cadence_info["skipped_unavailable_actor"]
    info["buckets"] = {"/".join(category): len(samples)
                       for category, samples in buckets.items()}
    tasks = args.task_schedule.split(",")
    if args.steps_per_chunk % len(tasks):
        raise ValueError("natural event schedule needs complete task cycles")
    _event_counts = cadence_info["event_counts"]
    _event_seed = group_seed(group)
    _event_slots = args.steps_per_chunk * tasks.count("event") // len(tasks)
    return buckets, info


def _category_keys(buckets):
    result = _mixed_category_keys(buckets)
    if _event_counts is None or _event_seed is None or _event_slots is None:
        raise RuntimeError("natural event schedule was not initialized")
    result["event"] = natural_event_schedule(
        result["event"], _event_counts, _event_slots, seed=_event_seed)
    return result


stream._collect_group = _collect_group
stream._category_keys = _category_keys


if __name__ == "__main__":
    stream.main()
