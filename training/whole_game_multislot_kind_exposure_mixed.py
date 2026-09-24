"""Preserve common commands during a bounded exposure fine-tune.

Half of the steps follow the natural replay-kind frequency, half sample kinds
uniformly. A frozen copy of the original head limits unintended drift.
"""
from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path
import random

import torch
from torch.nn import functional as F

from .whole_game_multislot_kind_exposure_fit import (
    collect_features, evaluate, kind_logits)
from .whole_game_multislot_model import MultiSlotWholeGameModel


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=400, seed=42, per_kind_limit=40):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    if parent["best_step"] < 1 or parent["memory_reset_interval"] != 8:
        raise ValueError("actor-ranking parent is not a selected reset fit")
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
    for parameter in model.parameters():
        parameter.requires_grad_(False)
    train, train_info = collect_features(
        model, release, parent["train_game_ids"], device=device,
        per_kind_limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect_features(model, release, dev_ids, device=device, seed=seed)
    original_head = copy.deepcopy(model.slot_kind).eval()
    for parameter in original_head.parameters():
        parameter.requires_grad_(False)
    for parameter in model.slot_kind.parameters():
        parameter.requires_grad_(True)
    optimizer = torch.optim.AdamW(model.slot_kind.parameters(), lr=2e-5)
    baseline, baseline_by_kind = evaluate(model, dev, device)
    checkpoints = [dict(step=0, overall=baseline, by_kind=baseline_by_kind)]
    best_step = 0
    best_score = -1
    best_state = None
    kinds = sorted(train)
    natural_weights = [train_info["seen"][kind] for kind in kinds]
    supported = model.supported_kind
    for step in range(1, steps + 1):
        kind = (rng.choice(kinds) if step % 2 else
                rng.choices(kinds, weights=natural_weights)[0])
        state, chosen_actor, true_actor, target, _, _ = rng.choice(train[kind])
        predicted = kind_logits(model, state, chosen_actor, device)
        replay_actor = kind_logits(model, state, true_actor, device)
        with torch.no_grad():
            original_features = torch.cat((state, chosen_actor)).to(
                device=device, dtype=torch.float32)
            original = original_head(original_features)[supported]
        target_tensor = torch.tensor([target], device=device)
        loss = (F.cross_entropy(predicted[None], target_tensor) +
                0.5 * F.cross_entropy(replay_actor[None], target_tensor) +
                F.kl_div(F.log_softmax(predicted[supported], dim=-1),
                         F.softmax(original, dim=-1), reduction="sum"))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite mixed kind loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.slot_kind.parameters(), 1.0)
        optimizer.step()
        if step % 100 == 0 or step == steps:
            overall, by_kind = evaluate(model, dev, device)
            checkpoints.append(dict(step=step, overall=overall, by_kind=by_kind))
            combat = (by_kind["attack"]["kind_correct"] +
                      by_kind["attack_move"]["kind_correct"])
            score = overall["kind_correct"] + 2 * combat
            print(json.dumps(dict(step=step,
                                  kind_correct=overall["kind_correct"],
                                  combat_kind_correct=combat)), flush=True)
            if (overall["kind_correct"] >= baseline["kind_correct"] - 30 and
                    combat > 0 and score > best_score):
                best_score = score
                best_step = step
                best_state = {name: tensor.detach().cpu().clone()
                              for name, tensor in model.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-multislot-kind-exposure-mixed-v1",
        promotion_eligible=False,
        source_identity_sha256=saved["source_identity_sha256"],
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        feature_source_sha256=hashlib.sha256(
            Path(__file__).with_name("whole_game_multislot_kind_exposure_fit.py")
            .read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=parent["train_game_ids"],
        dev_game_ids=parent["dev_game_ids"], train=train_info, dev=dev_info,
        steps=steps, best_step=best_step, checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    (output / "run.json").write_text(json.dumps(spec, indent=2) + "\n",
                                      encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-multislot-fit-v1",
                        source_identity_sha256=saved["source_identity_sha256"],
                        experimental_parent_sha256=report["source_checkpoint_sha256"],
                        state_dict=best_state), output / "teacher.pt")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--steps", type=int, default=400)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.actor_rank_report,
                 args.output, device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          checkpoints=[dict(step=row["step"],
                                            overall=row["overall"])
                                       for row in result["checkpoints"]]), indent=2))
