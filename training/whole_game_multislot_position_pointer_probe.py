"""Bounded visible-entity pointer for first-slot position targets.

The backbone, actor, kind and mixture heads are frozen. Replay actor and kind
are supplied only to this diagnostic, which is not promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import copy
import hashlib
import json
import math
from pathlib import Path
import random
import statistics

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


class PositionPointer(nn.Module):
    def __init__(self, model):
        super().__init__()
        self.width = model.width
        self.key = copy.deepcopy(model.slot_target_key)
        self.query = copy.deepcopy(model.slot_target_query)
        self.gate = nn.Linear(model.width * 2 + 64, 1)

    def forward(self, argument, entities, visible):
        logits = (self.key(entities) * self.query(argument)).sum(-1) / \
            math.sqrt(self.width)
        return logits.masked_fill(~visible, -torch.inf), self.gate(argument).squeeze(-1)


def collect_features(model, release, game_ids, *, device,
                     per_kind_limit=None, seed=42, reset_interval=8):
    selected = {game_id for ids in game_ids.values() for game_id in ids}
    buckets = defaultdict(list)
    seen = Counter()
    games = Counter()
    rng = random.Random(seed)
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                release, "train", game_ids=selected):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor([terrain.shape[1] * 32,
                                     terrain.shape[0] * 32], device=device)
            memory = None
            for index, sequence in enumerate(
                    cadence_sequences(trajectory_shard(directory))):
                if index % reset_interval == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                batch = {name: value.to(device) for name, value in batch.items()}
                base = model(batch, memory)
                memory = base["memory"]
                labels = sequence["labels"]
                if not labels or not sequence["actor_available"][0]:
                    continue
                label = labels[0]
                if not label["loss_masks"]["target_position"]:
                    continue
                actors = sorted(set(label["actor_positive"]) & ids.keys())
                if not actors:
                    continue
                kind = label["actions"]["kind"]
                entities = model._entities(batch)[0]
                visible = (batch["entity_mask"][0] &
                           ((batch["relation"][0] == 0) |
                            (batch["entity_numeric"][0, :, 4] > 0.5)))
                actor = entities[ids[actors[0]]]
                kind_token = model.slot_kind_context(
                    torch.tensor(KINDS.index(kind), device=device))
                argument = torch.cat((memory[0], actor, kind_token))
                parameters = model.slot_position(argument).reshape(
                    model.mixture_components, 5)
                means = parameters[:, 1:3].sigmoid()
                mixture = means[int(parameters[:, 0].argmax())]
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target = torch.tensor(label["actions"]["target_position"],
                                      device=device, dtype=torch.float32) * factor
                positions = batch["entity_numeric"][0, :, :2] * map_size
                distances = torch.linalg.vector_norm(positions - target, dim=-1)
                distances = distances.masked_fill(~visible, torch.inf)
                nearest = int(distances.argmin())
                anchor = float(distances[nearest]) <= 64
                sample = (argument.detach().cpu().half(),
                          entities.detach().cpu().half(),
                          visible.detach().cpu(),
                          positions.detach().cpu(), target.detach().cpu(),
                          (mixture * map_size).detach().cpu(),
                          nearest if anchor else -1)
                seen[kind] += 1
                bucket = buckets[kind]
                if per_kind_limit is None or len(bucket) < per_kind_limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < per_kind_limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(game_ids[matchup]) for matchup in game_ids):
        raise ValueError("position-pointer feature cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               retained={kind: len(items)
                                         for kind, items in buckets.items()})


def on_device(sample, device):
    argument, entities, visible, positions, target, mixture, nearest = sample
    return (argument.to(device=device, dtype=torch.float32),
            entities.to(device=device, dtype=torch.float32),
            visible.to(device=device), positions.to(device=device),
            target.to(device=device), mixture.to(device=device), nearest)


def evaluate(head, buckets, device):
    errors = defaultdict(list)
    gates = Counter()
    with torch.inference_mode():
        for samples in buckets.values():
            for sample in samples:
                argument, entities, visible, positions, target, mixture, nearest = \
                    on_device(sample, device)
                logits, gate = head(argument, entities, visible)
                candidate = positions[int(logits.argmax())]
                use_pointer = float(gate.sigmoid()) >= 0.5
                predictions = dict(mixture=mixture, pointer=candidate,
                                   gated=candidate if use_pointer else mixture)
                gates["samples"] += 1
                gates["anchor_labels"] += nearest >= 0
                gates["pointer_selected"] += use_pointer
                gates["gate_correct"] += use_pointer == (nearest >= 0)
                for name, prediction in predictions.items():
                    errors[name].append(float(torch.linalg.vector_norm(
                        prediction - target)))
    return dict(gates=gates, modes={
        name: dict(samples=len(values), within_64px=sum(value <= 64 for value in values),
                   median_error_px=statistics.median(values))
        for name, values in errors.items()})


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=1000, seed=42, per_kind_limit=250):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    torch.manual_seed(seed)
    rng = random.Random(seed)
    train, train_info = collect_features(
        model, release, parent["train_game_ids"], device=device,
        per_kind_limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect_features(model, release, dev_ids, device=device, seed=seed)
    head = PositionPointer(model).to(device).train()
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(head, dev, device))]
    best_hits = checkpoints[0]["metrics"]["modes"]["mixture"]["within_64px"]
    best_step = 0
    best_state = None
    kinds = sorted(train)
    natural_weights = [train_info["seen"][kind] for kind in kinds]
    for step in range(1, steps + 1):
        kind = (rng.choice(kinds) if step % 4 == 0 else
                rng.choices(kinds, weights=natural_weights)[0])
        sample = rng.choice(train[kind])
        argument, entities, visible, _, _, _, nearest = on_device(sample, device)
        logits, gate = head(argument, entities, visible)
        anchor = nearest >= 0
        loss = F.binary_cross_entropy_with_logits(
            gate, torch.tensor(float(anchor), device=device))
        if anchor:
            loss = loss + F.cross_entropy(
                logits[None], torch.tensor([nearest], device=device))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite position-pointer loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 200 == 0 or step == steps:
            metrics = evaluate(head, dev, device)
            checkpoints.append(dict(step=step, metrics=metrics))
            hits = metrics["modes"]["gated"]["within_64px"]
            print(json.dumps(dict(step=step, metrics=metrics)), flush=True)
            if hits > best_hits:
                best_hits = hits
                best_step = step
                best_state = {name: tensor.detach().cpu().clone()
                              for name, tensor in head.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-multislot-position-pointer-probe-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=parent["train_game_ids"],
        dev_game_ids=parent["dev_game_ids"], train=train_info, dev=dev_info,
        steps=steps, best_step=best_step, checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-position-pointer-head-v1",
                        source_checkpoint_sha256=report["source_checkpoint_sha256"],
                        state_dict=best_state), output / "head.pt")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--steps", type=int, default=1000)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.actor_rank_report,
                 args.output, device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          checkpoints=[dict(step=row["step"],
                                            metrics=row["metrics"]["modes"])
                                       for row in result["checkpoints"]]), indent=2))
