"""Bounded actor-relative geometry and hard-negative combat probe.

The actor-ranked encoder is frozen. This separate first-slot head is trained
only on train games omitted from the full fit and checked on disjoint omitted
train games. It is not causal validation or promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random

import torch
from torch import nn

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_combat_pair_probe import COMBAT, source_prediction
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


THRESHOLDS = (0., 1., 2., 3., 4., 5., 6.)


class GeometryCombatHead(nn.Module):
    def __init__(self, width):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(width * 2 + 11, 192), nn.GELU(),
                                 nn.Linear(192, 96), nn.GELU(), nn.Linear(96, 3))

    def forward(self, state, entities, geometry, own):
        repeated = state.expand(entities.shape[0], -1)
        logits = self.net(torch.cat((repeated, entities, geometry), -1))
        return logits.masked_fill(~own[:, None], -torch.inf)


def actor_geometry(batch, map_size):
    numeric = batch["entity_numeric"][0]
    relation = batch["relation"][0]
    xy = numeric[:, :2]
    enemy = (relation == 1) & (numeric[:, 4] > 0.5)
    own = relation == 0
    output = torch.zeros((xy.shape[0], 11), device=xy.device)
    output[:, 6:8] = xy
    output[:, 8:10] = numeric[:, 8:10]
    output[:, 10] = enemy.sum().clamp(max=32) / 32
    if enemy.any():
        delta = (xy[:, None, :] - xy[enemy][None, :, :]) * map_size
        distances = torch.linalg.vector_norm(delta, dim=-1)
        nearest, index = distances.min(-1)
        output[:, 0] = torch.log1p(nearest).clamp(max=math.log1p(2048)) / math.log1p(2048)
        output[:, 1] = (distances <= 128).sum(-1).clamp(max=16) / 16
        output[:, 2] = (distances <= 256).sum(-1).clamp(max=16) / 16
        output[:, 3] = (distances <= 512).sum(-1).clamp(max=16) / 16
        nearest_delta = delta[torch.arange(xy.shape[0], device=xy.device), index]
        output[:, 4:6] = (nearest_delta / 512).clamp(-4, 4)
    else:
        nearest = torch.full((xy.shape[0],), torch.inf, device=xy.device)
        output[:, 0] = 1
    return output, own, nearest


def collect(model, release, game_ids, *, device, limit=None, seed=42):
    chosen = {game_id for ids in game_ids.values() for game_id in ids}
    rng = random.Random(seed)
    buckets = defaultdict(list)
    seen, games, hard_seen = Counter(), Counter(), Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(release, "train", game_ids=chosen):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor((terrain.shape[1] * 32, terrain.shape[0] * 32),
                                    device=device, dtype=torch.float32)
            memory = None
            for index, sequence in enumerate(cadence_sequences(trajectory_shard(directory))):
                if index % 8 == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                batch = {name: value.to(device) for name, value in batch.items()}
                memory = model(batch, memory)["memory"]
                if not sequence["labels"] or not sequence["actor_available"][0]:
                    continue
                label = sequence["labels"][0]
                positives = set(label["actor_positive"]) & ids.keys()
                if not positives:
                    continue
                kind = label["actions"]["kind"]
                if not bool(model.supported_kind[KINDS.index(kind)]):
                    continue
                geometry, own, nearest = actor_geometry(batch, map_size)
                positive = torch.zeros_like(own)
                positive[[ids[token] for token in positives]] = True
                if not (positive & own).any():
                    raise ValueError("confirmed actor is not owned")
                hard = bool((nearest[positive] <= 256).any())
                hard_seen[kind] += hard
                sample = (memory[0].detach().cpu().half(),
                          model._entities(batch)[0].detach().cpu().half(),
                          geometry.detach().cpu().half(), own.cpu(), positive.cpu(),
                          KINDS.index(kind), hard)
                seen[kind] += 1
                bucket = buckets[kind]
                if limit is None or len(bucket) < limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(ids) for matchup, ids in game_ids.items()):
        raise ValueError("geometry-combat cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               hard_seen=dict(hard_seen),
                               retained={kind: len(rows) for kind, rows in buckets.items()})


def on_device(sample, device):
    state, entities, geometry, own, positive, kind, hard = sample
    return (state.to(device=device, dtype=torch.float32),
            entities.to(device=device, dtype=torch.float32),
            geometry.to(device=device, dtype=torch.float32),
            own.to(device), positive.to(device), kind, hard)


def evaluate(model, head, buckets, device):
    baseline = Counter()
    results = {str(threshold): Counter() for threshold in THRESHOLDS}
    by_kind = {str(threshold): {} for threshold in THRESHOLDS}
    with torch.inference_mode():
        for name, samples in sorted(buckets.items()):
            local = {str(threshold): Counter() for threshold in THRESHOLDS}
            for sample in samples:
                state, entities, geometry, own, positive, true_kind, _ = on_device(
                    sample, device)
                source_actor, source_kind = source_prediction(model, state, entities, own)
                baseline["samples"] += 1
                baseline["kind_correct"] += source_kind == true_kind
                baseline["pair_correct"] += (source_kind == true_kind and
                                             bool(positive[source_actor]))
                scores = head(state, entities, geometry, own)
                noncombat = float(scores[:, 0].max())
                combat_flat = scores[:, 1:].flatten()
                combat_index = int(combat_flat.argmax())
                combat_actor, combat_kind = divmod(combat_index, 2)
                combat_kind = KINDS.index(COMBAT[combat_kind])
                margin = float(combat_flat[combat_index]) - noncombat
                for threshold in THRESHOLDS:
                    counts = local[str(threshold)]
                    override = margin > threshold
                    actor = combat_actor if override else source_actor
                    kind = combat_kind if override else source_kind
                    counts["samples"] += 1
                    counts["overrides"] += override
                    counts["false_combat"] += override and name not in COMBAT
                    counts["combat_detected"] += override and name in COMBAT
                    counts["kind_correct"] += kind == true_kind
                    counts["pair_correct"] += kind == true_kind and bool(positive[actor])
                    counts["combat_kind_correct"] += (name in COMBAT and
                                                      kind == true_kind)
                    counts["combat_pair_correct"] += (name in COMBAT and
                                                      kind == true_kind and
                                                      bool(positive[actor]))
            for threshold in THRESHOLDS:
                key = str(threshold)
                results[key].update(local[key])
                by_kind[key][name] = dict(local[key])
    return dict(baseline=dict(baseline), thresholds={
        key: dict(overall=dict(counts), by_kind=by_kind[key])
        for key, counts in results.items()})


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=1800, seed=42, per_kind_limit=200):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    for parameter in model.parameters():
        parameter.requires_grad_(False)
    torch.set_num_threads(2)
    torch.manual_seed(seed)
    rng = random.Random(seed)
    train, train_info = collect(model, release, parent["train_game_ids"],
                                device=device, limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect(model, release, dev_ids, device=device, seed=seed)
    hard_right_click = [sample for sample in train["right_click"] if sample[-1]]
    if not hard_right_click:
        raise ValueError("no hard right-click negatives")
    combat_kinds = [kind for kind in COMBAT if kind in train]
    other_kinds = [kind for kind in train if kind not in COMBAT]
    combat_weights = [train_info["seen"][kind] for kind in combat_kinds]
    other_weights = [train_info["seen"][kind] for kind in other_kinds]
    head = GeometryCombatHead(model.width).to(device)
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(model, head, dev, device))]
    base = checkpoints[0]["metrics"]["baseline"]
    best_step, best_threshold, best_score, best_state = 0, None, -1, None
    for step in range(1, steps + 1):
        phase = step % 4
        if phase in (0, 1):
            kind = rng.choices(combat_kinds, weights=combat_weights)[0]
            sample = rng.choice(train[kind])
        elif phase == 2:
            sample = rng.choice(hard_right_click)
            kind = "right_click"
        else:
            kind = rng.choices(other_kinds, weights=other_weights)[0]
            sample = rng.choice(train[kind])
        state, entities, geometry, own, positive, _, _ = on_device(sample, device)
        label = COMBAT.index(kind) + 1 if kind in COMBAT else 0
        scores = head(state, entities, geometry, own)
        loss = torch.logsumexp(scores[own].flatten(), 0) - torch.logsumexp(
            scores[positive, label], 0)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite geometry combat loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 300 == 0 or step == steps:
            metrics = evaluate(model, head, dev, device)
            checkpoints.append(dict(step=step, metrics=metrics))
            compact = {}
            for threshold, row in metrics["thresholds"].items():
                counts = row["overall"]
                compact[threshold] = {key: counts.get(key, 0) for key in (
                    "combat_kind_correct", "combat_pair_correct", "false_combat",
                    "kind_correct", "pair_correct")}
                if (counts.get("combat_pair_correct", 0) > 0 and
                        counts["kind_correct"] >= base["kind_correct"] - 30 and
                        counts["pair_correct"] >= base["pair_correct"] - 30 and
                        counts.get("false_combat", 0) <= 30):
                    score = counts["combat_pair_correct"] * 4 + counts["pair_correct"]
                    if score > best_score:
                        best_score, best_step, best_threshold = score, step, float(threshold)
                        best_state = {name: value.detach().cpu().clone()
                                      for name, value in head.state_dict().items()}
            print(json.dumps(dict(step=step, baseline=base, thresholds=compact)), flush=True)
    report = dict(
        schema="protodd-whole-game-combat-geometry-probe-v1",
        promotion_eligible=False,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        pair_source_sha256=hashlib.sha256(Path(__file__).with_name(
            "whole_game_multislot_combat_pair_probe.py").read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=parent["train_game_ids"],
        dev_game_ids=parent["dev_game_ids"], train=train_info, dev=dev_info,
        steps=steps, thresholds=THRESHOLDS, best_step=best_step,
        best_threshold=best_threshold, checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-combat-geometry-head-v1",
                        source_checkpoint_sha256=report["source_checkpoint_sha256"],
                        threshold=best_threshold, state_dict=best_state), output / "head.pt")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--steps", type=int, default=1800)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.actor_rank_report, args.output,
                 device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          best_threshold=result["best_threshold"]), indent=2))
