"""Bounded map-aware refinement of the frozen mixture position decoder.

Replay actor and kind are supplied for diagnosis only. The full-fit backbone
and its mixture are frozen; this probe cannot establish causal command quality.
"""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import random

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_multislot_spatial_target_probe import (
    SIDE, collect, on_device, summary)


class MixtureAnchoredHead(nn.Module):
    def __init__(self, argument_width, hidden=96, prior_sigma_px=256):
        super().__init__()
        self.hidden = hidden
        self.prior_sigma_px = prior_sigma_px
        self.key = nn.Sequential(nn.Linear(16, hidden), nn.ReLU(),
                                 nn.Linear(hidden, hidden))
        self.query = nn.Sequential(nn.Linear(argument_width, hidden), nn.LayerNorm(hidden),
                                   nn.ReLU(), nn.Linear(hidden, hidden))
        nn.init.zeros_(self.query[-1].weight)
        nn.init.zeros_(self.query[-1].bias)
        axis = (torch.arange(SIDE, dtype=torch.float32) + 0.5) / SIDE
        yy, xx = torch.meshgrid(axis, axis, indexing="ij")
        self.register_buffer("xy", torch.stack((xx, yy), -1).reshape(-1, 2))

    def forward(self, argument, actor_xy, spatial, map_size, mixture):
        mixture_xy = mixture / map_size
        delta_actor = self.xy - actor_xy
        delta_mixture = self.xy - mixture_xy
        trig = torch.cat((torch.sin(2 * torch.pi * self.xy),
                          torch.cos(2 * torch.pi * self.xy)), -1)
        cells = torch.cat((self.xy, delta_actor, delta_mixture,
                           torch.linalg.vector_norm(delta_actor, dim=-1, keepdim=True),
                           torch.linalg.vector_norm(delta_mixture, dim=-1, keepdim=True),
                           trig, spatial.permute(1, 2, 0).reshape(-1, 4)), -1)
        correction = (self.key(cells) * self.query(argument)).sum(-1) / self.hidden ** 0.5
        prior = -0.5 * ((delta_mixture * map_size).square().sum(-1) /
                        self.prior_sigma_px ** 2)
        return prior + correction


def evaluate(head, buckets, device):
    from collections import defaultdict
    errors = defaultdict(list)
    by_kind = {}
    with torch.inference_mode():
        for kind, samples in sorted(buckets.items()):
            local = defaultdict(list)
            for sample in samples:
                argument, actor_xy, spatial, map_size, target, mixture = on_device(
                    sample, device)
                logits = head(argument, actor_xy, spatial, map_size, mixture)
                point = head.xy[int(logits.argmax())] * map_size
                for name, prediction in (("mixture", mixture), ("refined", point)):
                    error = float(torch.linalg.vector_norm(prediction - target))
                    errors[name].append(error)
                    local[name].append(error)
            by_kind[kind] = {name: summary(values) for name, values in local.items()}
    return dict(overall={name: summary(values) for name, values in errors.items()},
                by_kind=by_kind)


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
    torch.set_num_threads(2)
    torch.manual_seed(seed)
    rng = random.Random(seed)
    train, train_info = collect(model, release, parent["train_game_ids"],
                                device=device, limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect(model, release, dev_ids, device=device, seed=seed)
    head = MixtureAnchoredHead(model.width * 2 + 64).to(device)
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(head, dev, device))]
    baseline = checkpoints[0]["metrics"]["overall"]["mixture"]
    best_step, best_state, best_hits = 0, None, baseline["within_64px"]
    kinds = sorted(train)
    weights = [train_info["seen"][kind] for kind in kinds]
    for step in range(1, steps + 1):
        kind = (rng.choice(kinds) if step % 4 == 0 else
                rng.choices(kinds, weights=weights)[0])
        argument, actor_xy, spatial, map_size, target, mixture = on_device(
            rng.choice(train[kind]), device)
        xy = (target / map_size).clamp(0, 1 - 1e-6)
        cell = int(xy[1] * SIDE) * SIDE + int(xy[0] * SIDE)
        logits = head(argument, actor_xy, spatial, map_size, mixture)
        loss = F.cross_entropy(logits[None], torch.tensor([cell], device=device))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite spatial refinement loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 300 == 0 or step == steps:
            metrics = evaluate(head, dev, device)
            checkpoints.append(dict(step=step, metrics=metrics))
            current = metrics["overall"]["refined"]
            print(json.dumps(dict(step=step, metrics=metrics["overall"])), flush=True)
            if (current["within_64px"] > best_hits and
                    current["median_error_px"] <= baseline["median_error_px"]):
                best_hits, best_step = current["within_64px"], step
                best_state = {name: value.detach().cpu().clone()
                              for name, value in head.state_dict().items()}
    dependency = Path(__file__).with_name("whole_game_multislot_spatial_target_probe.py")
    report = dict(
        schema="protodd-whole-game-spatial-refinement-probe-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        feature_source_sha256=hashlib.sha256(dependency.read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        grid_side=SIDE, prior_sigma_px=256, memory_reset_interval=8,
        train_game_ids=parent["train_game_ids"], dev_game_ids=parent["dev_game_ids"],
        train=train_info, dev=dev_info, steps=steps, best_step=best_step,
        checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-spatial-refinement-head-v1",
                        source_checkpoint_sha256=report["source_checkpoint_sha256"],
                        state_dict=best_state), output / "head.pt")
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
                          checkpoints=[dict(step=row["step"],
                                            overall=row["metrics"]["overall"])
                                       for row in result["checkpoints"]]), indent=2))
