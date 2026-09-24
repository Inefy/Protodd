"""Bounded spatial target probe on games omitted from the source fit.

Replay actor and kind are supplied for this position-only diagnostic. The
backbone remains frozen, and this is not causal or promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
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


SIDE = 64


class SpatialTargetHead(nn.Module):
    def __init__(self, argument_width, hidden=96):
        super().__init__()
        self.query = nn.Sequential(nn.Linear(argument_width, hidden), nn.LayerNorm(hidden),
                                   nn.ReLU(), nn.Linear(hidden, hidden))
        self.key = nn.Sequential(nn.Linear(13, hidden), nn.ReLU(),
                                 nn.Linear(hidden, hidden))
        axis = (torch.arange(SIDE, dtype=torch.float32) + 0.5) / SIDE
        yy, xx = torch.meshgrid(axis, axis, indexing="ij")
        self.register_buffer("xy", torch.stack((xx, yy), -1).reshape(-1, 2))

    def forward(self, argument, actor_xy, spatial):
        delta = self.xy - actor_xy
        trig = torch.cat((torch.sin(2 * torch.pi * self.xy),
                          torch.cos(2 * torch.pi * self.xy)), -1)
        cells = torch.cat((self.xy, delta, torch.linalg.vector_norm(
            delta, dim=-1, keepdim=True), trig,
            spatial.permute(1, 2, 0).reshape(-1, 4)), -1)
        return (self.key(cells) * self.query(argument)).sum(-1) / 96 ** 0.5


def spatial_grid(batch):
    grid = F.interpolate(batch["spatial"], size=(SIDE, SIDE), mode="bilinear",
                         align_corners=False)[0, :3]
    occupancy = torch.zeros((1, SIDE, SIDE), device=grid.device)
    present = batch["entity_mask"][0] & ((batch["relation"][0] == 0) |
              (batch["entity_numeric"][0, :, 4] > 0.5))
    xy = batch["entity_numeric"][0, present, :2].clamp(0, 1 - 1e-6)
    if xy.numel():
        x = (xy[:, 0] * SIDE).long()
        y = (xy[:, 1] * SIDE).long()
        occupancy[0].index_put_((y, x), torch.ones_like(x, dtype=grid.dtype),
                                accumulate=True)
    return torch.cat((grid, occupancy.clamp(max=4) / 4), 0).detach().cpu().half()


def collect(model, release, game_ids, *, device, limit=None, seed=42):
    chosen = {game_id for ids in game_ids.values() for game_id in ids}
    buckets = defaultdict(list)
    seen, games = Counter(), Counter()
    rng = random.Random(seed)
    with torch.inference_mode():
        for record, directory, _ in selected_shards(release, "train", game_ids=chosen):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor([terrain.shape[1] * 32, terrain.shape[0] * 32],
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
                if not label["loss_masks"]["target_position"]:
                    continue
                actors = sorted(set(label["actor_positive"]) & ids.keys())
                if not actors:
                    continue
                kind = label["actions"]["kind"]
                actor_index = ids[actors[0]]
                actor = model._entities(batch)[0, actor_index]
                token = model.slot_kind_context(torch.tensor(KINDS.index(kind), device=device))
                argument = torch.cat((memory[0], actor, token))
                parameters = model.slot_position(argument).reshape(
                    model.mixture_components, 5)
                mixture = parameters[int(parameters[:, 0].argmax()), 1:3].sigmoid()
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target = torch.tensor(label["actions"]["target_position"],
                                      device=device, dtype=torch.float32) * factor
                sample = (argument.detach().cpu().half(),
                          batch["entity_numeric"][0, actor_index, :2].detach().cpu(),
                          spatial_grid(batch), map_size.cpu(), target.cpu(),
                          (mixture * map_size).detach().cpu())
                seen[kind] += 1
                bucket = buckets[kind]
                if limit is None or len(bucket) < limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(ids) for matchup, ids in game_ids.items()):
        raise ValueError("spatial target cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               retained={kind: len(rows) for kind, rows in buckets.items()})


def on_device(sample, device):
    argument, actor_xy, spatial, map_size, target, mixture = sample
    return (argument.to(device=device, dtype=torch.float32), actor_xy.to(device),
            spatial.to(device=device, dtype=torch.float32), map_size.to(device),
            target.to(device), mixture.to(device))


def evaluate(head, buckets, device):
    errors = defaultdict(list)
    by_kind = {}
    with torch.inference_mode():
        for kind, samples in sorted(buckets.items()):
            kind_errors = defaultdict(list)
            for sample in samples:
                argument, actor_xy, spatial, map_size, target, mixture = on_device(
                    sample, device)
                cell = head.xy[int(head(argument, actor_xy, spatial).argmax())]
                for name, point in (("mixture", mixture), ("spatial", cell * map_size)):
                    error = float(torch.linalg.vector_norm(point - target))
                    errors[name].append(error)
                    kind_errors[name].append(error)
            by_kind[kind] = {name: summary(values) for name, values in kind_errors.items()}
    return dict(overall={name: summary(values) for name, values in errors.items()},
                by_kind=by_kind)


def summary(values):
    return dict(samples=len(values), within_64px=sum(value <= 64 for value in values),
                median_error_px=statistics.median(values))


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=1200, seed=42, per_kind_limit=200):
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
    torch.set_num_threads(2)
    torch.manual_seed(seed)
    rng = random.Random(seed)
    train, train_info = collect(model, release, parent["train_game_ids"],
                                device=device, limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect(model, release, dev_ids, device=device, seed=seed)
    head = SpatialTargetHead(model.width * 2 + 64).to(device)
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(head, dev, device))]
    best_hits = checkpoints[0]["metrics"]["overall"]["mixture"]["within_64px"]
    best_step, best_state = 0, None
    kinds = sorted(train)
    natural_weights = [train_info["seen"][kind] for kind in kinds]
    for step in range(1, steps + 1):
        kind = (rng.choice(kinds) if step % 4 == 0 else
                rng.choices(kinds, weights=natural_weights)[0])
        argument, actor_xy, spatial, map_size, target, _ = on_device(
            rng.choice(train[kind]), device)
        xy = (target / map_size).clamp(0, 1 - 1e-6)
        cell = int(xy[1] * SIDE) * SIDE + int(xy[0] * SIDE)
        logits = head(argument, actor_xy, spatial)
        loss = F.cross_entropy(logits[None], torch.tensor([cell], device=device))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite spatial target loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 300 == 0 or step == steps:
            metrics = evaluate(head, dev, device)
            checkpoints.append(dict(step=step, metrics=metrics))
            hits = metrics["overall"]["spatial"]["within_64px"]
            print(json.dumps(dict(step=step, metrics=metrics["overall"])), flush=True)
            if hits > best_hits:
                best_hits, best_step = hits, step
                best_state = {name: value.detach().cpu().clone()
                              for name, value in head.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-spatial-target-probe-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        grid_side=SIDE, memory_reset_interval=8,
        train_game_ids=parent["train_game_ids"], dev_game_ids=parent["dev_game_ids"],
        train=train_info, dev=dev_info, steps=steps, best_step=best_step,
        checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-spatial-target-head-v1",
                        source_checkpoint_sha256=report["source_checkpoint_sha256"],
                        state_dict=best_state), output / "head.pt")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--steps", type=int, default=1200)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.actor_rank_report, args.output,
                 device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          checkpoints=[dict(step=row["step"],
                                            overall=row["metrics"]["overall"])
                                       for row in result["checkpoints"]]), indent=2))
