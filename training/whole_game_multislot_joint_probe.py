"""Test actor/kind joint decoding on train games omitted from the fit.

This is a bounded decoder experiment, not a causal or promotion audit.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path

import torch
from torch.nn import functional as F

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def probe(checkpoint, release, output, *, device="cuda", reset_interval=8):
    checkpoint, release, output = map(Path, (checkpoint, release, output))
    if output.exists() or reset_interval < 1:
        raise ValueError("existing output or invalid reset interval")
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    trained = {game_id for group in spec["groups"]
               for ids in group.values() for game_id in ids}
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    chosen = {}
    for record in sorted(identity["selected"], key=lambda item: item["game_id"]):
        if (record["split"] == "train" and record["matchup"] not in chosen and
                record["game_id"] not in trained and
                (release / "games" / key(record) / "receipt.json").exists()):
            chosen[record["matchup"]] = record["game_id"]
    if set(chosen) != {"PvP", "PvT", "PvZ"}:
        raise ValueError("three omitted train games unavailable")
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    methods = ("actor_first", "joint_0.5", "joint_1", "joint_2", "joint_4")
    totals = {name: Counter() for name in methods}
    by_kind = {name: defaultdict(Counter) for name in methods}
    games = Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                release, "train", game_ids=set(chosen.values())):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            memory = None
            for sequence_index, sequence in enumerate(
                    cadence_sequences(trajectory_shard(directory))):
                if sequence_index % reset_interval == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                batch = {name: value.to(device) for name, value in batch.items()}
                base = model(batch, memory)
                memory = base["memory"]
                labels = sequence["labels"]
                if not labels or not sequence["actor_available"][0]:
                    continue
                label = labels[0]
                actors = set(label["actor_positive"]) & ids.keys()
                if not actors:
                    continue
                true_indices = {ids[actor] for actor in actors}
                true_kind = KINDS.index(label["actions"]["kind"])
                entities = model._entities(batch)
                own = batch["entity_mask"] & (batch["relation"] == 0)
                state = base["memory"]
                actor_logits = (model.slot_actor_key(entities) *
                                model.slot_actor_query(state)[:, None, :]).sum(-1) / math.sqrt(model.width)
                actor_logits = actor_logits.masked_fill(~own, -torch.inf)
                actor_state = torch.cat((state[:, None, :].expand_as(entities),
                                         entities), dim=-1)
                kind_logits = model.slot_kind(actor_state).masked_fill(
                    ~model.supported_kind[None, None, :], -torch.inf)
                actor_lp = F.log_softmax(actor_logits, dim=-1)
                kind_lp = F.log_softmax(kind_logits, dim=-1)
                actor_first = int(actor_logits[0].argmax())
                choices = {"actor_first": (
                    actor_first, int(kind_logits[0, actor_first].argmax()))}
                for weight in (0.5, 1, 2, 4):
                    joint = weight * actor_lp[:, :, None] + kind_lp
                    flat = int(joint[0].flatten().argmax())
                    choices[f"joint_{weight}"] = divmod(flat, len(KINDS))
                predicted_action = int(model.slot_stop(state).argmax(-1)[0]) == 0
                for name, (actor, kind) in choices.items():
                    for counter in (totals[name], by_kind[name][KINDS[true_kind]]):
                        counter["samples"] += 1
                        counter["predicted_action"] += predicted_action
                        counter["actor_correct"] += actor in true_indices
                        counter["kind_correct"] += kind == true_kind
                        counter["pair_correct"] += actor in true_indices and kind == true_kind
                        counter["active_pair_correct"] += (
                            predicted_action and actor in true_indices and kind == true_kind)
    if len(games) != 3 or any(count != 1 for count in games.values()):
        raise ValueError("omitted train cohort incomplete")
    report = dict(
        schema="protodd-whole-game-multislot-joint-probe-v1",
        promotion_eligible=False,
        cohort="train games omitted from 2400-per-matchup fit",
        games=dict(games), game_ids=chosen, memory_reset_interval=reset_interval,
        checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        overall={name: dict(count) for name, count in totals.items()},
        by_true_kind={name: {kind: dict(count) for kind, count in sorted(kinds.items())}
                      for name, kinds in by_kind.items()})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    result = probe(args.checkpoint, args.release, args.output, device=args.device)
    print(json.dumps(result["overall"], indent=2))
