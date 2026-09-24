"""Canonicalize repeated replay-selection IDs with identical actor evidence.

Some valid replays contain the same unit twice in one selected-unit list. A
duplicate is safe to collapse only when every repeated actor-effect record is
identical. Conflicting evidence remains a hard extraction error.
"""
from __future__ import annotations


def canonicalize_actor_selection(command):
    selected = command["selected_own"]
    effects = command["semantic"]["actor_effects"]
    unique_selected = list(dict.fromkeys(selected))
    unique_effects = {}
    for effect in effects:
        actor_id = effect["id"]
        previous = unique_effects.get(actor_id)
        if previous is not None and previous != effect:
            raise ValueError("conflicting duplicate actor transition evidence")
        unique_effects[actor_id] = effect
    if len(unique_selected) == len(selected) and len(unique_effects) == len(effects):
        return command, False
    result = dict(command)
    result["selected_own"] = unique_selected
    result["semantic"] = dict(command["semantic"], actor_effects=list(unique_effects.values()))
    return result, True
