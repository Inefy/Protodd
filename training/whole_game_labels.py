"""Causal, partially masked imitation labels from native command-state evidence.

These are candidates for offline experiments. They do not certify original-game
parity, command completion, winning play, or live deployment readiness.
"""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from .whole_game_actor_dedupe import canonicalize_actor_selection

COMMAND_SCHEMA = "protodd-command-v1"
LABEL_SCHEMA = "protodd-imitation-label-v1"


def observation_reference(row):
    return dict(frame=row["frame"], perspective=row["perspective"], sequence=row["sequence"],
                own={e["id"]: e["own_state"]["order"] for e in row["entities"] if e["relation"] == 0},
                visible={e["id"] for e in row["entities"] if e["visible"]})


def label_command(row, reference, width, height):
    """Check evidence, then construct labels with an explicit mask for every head."""
    if (row.get("schema") != COMMAND_SCHEMA or row["frame"] != reference["frame"]
            or row["perspective"] != reference["perspective"]
            or row["observation_sequence"] != reference["sequence"]
            or type(row["engine_returned"]) is not bool
            or len(set(row["selected_own"])) != len(row["selected_own"])
            or not set(row["selected_own"]).issubset(reference["own"])):
        raise ValueError("command lacks causal own-actor observation")
    semantic = row["semantic"]
    effects = semantic["actor_effects"]
    effect_ids = {a["id"] for a in effects}
    if len(effect_ids) != len(effects) or not effect_ids.issubset(reference["own"]):
        raise ValueError("invalid effect actors")
    changed = set()
    for effect in effects:
        if (type(effect["changed"]) is not bool or type(effect["removed"]) is not bool
                or effect["before_order"] != reference["own"][effect["id"]]
                or (effect["removed"] and not effect["changed"])):
            raise ValueError("invalid actor transition evidence")
        if effect["changed"]:
            changed.add(effect["id"])
    for flag in ("decoded", "target_requested", "target_available", "queued"):
        if type(semantic[flag]) is not bool:
            raise ValueError("semantic flags must be booleans")
    if semantic["target_available"]:
        if not semantic["target_requested"] or semantic["target"] not in reference["visible"]:
            raise ValueError("target is not a current legal observation")
    elif semantic["target"] != -1:
        raise ValueError("unavailable target must be masked")
    if not semantic["decoded"]:
        expected = "not_applicable"
    elif changed:
        expected = "confirmed_transition"
    else:
        expected = "unconfirmed_no_change" if row["engine_returned"] else "engine_rejected"
    if row["acceptance"] != expected:
        raise ValueError("acceptance contradicts observed transitions")
    if expected != "confirmed_transition":
        return None
    if semantic["domain"] not in ("unit_control", "production", "economy", "ability", "transport"):
        raise ValueError("unknown unit-control domain")
    if semantic["coordinate_space"] not in ("pixel", "build_tile"):
        raise ValueError("unknown target coordinate space")
    position = semantic["position"]
    if position is not None:
        scale = 32 if semantic["coordinate_space"] == "build_tile" else 1
        if (len(position) != 2 or any(type(v) is not int for v in position)
                or not 0 <= position[0] * scale < width or not 0 <= position[1] * scale < height):
            raise ValueError("confirmed command has out-of-map target")
    fields = {name: semantic[name] for name in ("kind", "order", "unit_type", "technology", "upgrade", "queue_slot", "queued")}
    target_known = not semantic["target_requested"] or semantic["target_available"]
    target_mode = ("entity" if semantic["target_requested"] else "position" if position is not None else "none")
    fields.update(target_mode=target_mode if target_known else None,
                  target_entity=semantic["target"] if semantic["target_available"] else None,
                  target_position=position if target_mode == "position" else None)
    masks = dict(kind=True, actors=True, queued=True, target_mode=target_known,
                 target_entity=semantic["target_available"],
                 target_position=target_mode == "position" and position is not None)
    for name in ("order", "unit_type", "technology", "upgrade", "queue_slot"):
        if type(fields[name]) is not int or fields[name] < -1:
            raise ValueError("invalid categorical argument")
        masks[name] = fields[name] != -1
        if not masks[name]:
            fields[name] = None
    # A mixed selection may contain a unit which rejected the command. Mask its
    # actor loss; do not tell the model it was a successful actor or a negative.
    return dict(schema=LABEL_SCHEMA, perspective=row["perspective"], frame=row["frame"],
                observation_sequence=row["observation_sequence"], domain=semantic["domain"],
                actions=fields, loss_masks=masks, actor_positive=sorted(changed),
                actor_unknown=sorted(effect_ids - changed),
                actor_negative=sorted(set(reference["own"]) - effect_ids),
                coordinate_space=semantic["coordinate_space"],
                evidence="immediate_own_command_state_transition", command_completion_verified=False)


def materialize(extracted, output):
    extracted, output = Path(extracted), Path(output)
    summary = json.loads((extracted / "summary.json").read_text())
    if not summary.get("complete") or summary.get("command_schema") != COMMAND_SCHEMA:
        raise ValueError("incomplete or incompatible native extraction")
    counts, kinds, domains = Counter(), Counter(), Counter()
    with output.open("x", encoding="utf-8") as labels, (extracted / "commands.jsonl").open() as commands:
        with (extracted / "observations.jsonl").open() as observations:
            for line in observations:
                observation = json.loads(line)
                if observation["reason"] != "before_command":
                    continue
                command_line = commands.readline()
                if not command_line:
                    raise ValueError("missing command")
                command = json.loads(command_line)
                command, _ = canonicalize_actor_selection(command)
                candidate = label_command(command, observation_reference(observation),
                                          summary["width_tiles"] * 32, summary["height_tiles"] * 32)
                counts[command["acceptance"]] += 1
                if candidate is None:
                    continue
                labels.write(json.dumps(candidate, separators=(",", ":")) + "\n")
                kinds[candidate["actions"]["kind"]] += 1
                domains[candidate["domain"]] += 1
        if commands.readline():
            raise ValueError("extra unpaired command")
    return dict(command_evidence=counts, candidate_labels=sum(kinds.values()), kinds=kinds, domains=domains,
                training_ready=False, command_completion_verified=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("extracted", type=Path)
    parser.add_argument("output", type=Path)
    print(json.dumps(materialize(**vars(parser.parse_args())), indent=2))
