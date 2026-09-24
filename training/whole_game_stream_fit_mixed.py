"""GPU ablation blending replay-frequency and rare-kind action updates.

The model, replay cohort, initialization and structured objective match the
structured run. Only the action-category schedule changes.
"""
from __future__ import annotations

from . import whole_game_stream_fit_structured  # Installs the structured objective.
from . import whole_game_stream_fit as stream
from .whole_game_action_sampling import group_kind_counts, group_seed, mixed_action_schedule


stream.SCHEMA = "protodd-whole-game-stream-fit-mixed-v1"
stream.SOURCE_FILES = (*stream.SOURCE_FILES,
                       "whole_game_action_sampling.py", "whole_game_stream_fit_mixed.py")
_original_collect_group = stream._collect_group
_original_category_keys = stream._category_keys
_group_counts = None
_group_seed = None
_action_slots = None


def _collect_group(args, group, quality, seed):
    global _group_counts, _group_seed, _action_slots
    tasks = args.task_schedule.split(",")
    if args.steps_per_chunk % len(tasks):
        raise ValueError("mixed fit needs complete task-schedule cycles per group")
    result = _original_collect_group(args, group, quality, seed)
    _group_counts = group_kind_counts(args.release, group)
    _group_seed = group_seed(group)
    _action_slots = args.steps_per_chunk * tasks.count("action") // len(tasks)
    return result


def _category_keys(buckets):
    result = _original_category_keys(buckets)
    if _group_counts is None or _group_seed is None or _action_slots is None:
        raise RuntimeError("mixed action schedule was not initialized")
    result["action"] = mixed_action_schedule(result["action"], _group_counts,
                                              _action_slots, natural_share=0.5,
                                              seed=_group_seed)
    return result


stream._collect_group = _collect_group
stream._category_keys = _category_keys


if __name__ == "__main__":
    stream.main()
