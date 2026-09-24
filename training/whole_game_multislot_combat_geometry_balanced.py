"""Balance attack and attack-move in the bounded geometry combat probe."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import random

import torch

from .whole_game_multislot_combat_geometry_probe import (
    GeometryCombatHead, THRESHOLDS, collect, evaluate, on_device)
from .whole_game_multislot_model import MultiSlotWholeGameModel


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
    if not train.get("attack") or not train.get("attack_move") or not hard_right_click:
        raise ValueError("attack, attack-move and hard negatives required")
    other_kinds = [kind for kind in train if kind not in ("attack", "attack_move")]
    other_weights = [train_info["seen"][kind] for kind in other_kinds]
    head = GeometryCombatHead(model.width).to(device)
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(model, head, dev, device))]
    base = checkpoints[0]["metrics"]["baseline"]
    best_step, best_threshold, best_score, best_state = 0, None, -1, None
    for step in range(1, steps + 1):
        phase = step % 4
        if phase == 0:
            kind, sample = "attack", rng.choice(train["attack"])
        elif phase == 1:
            kind, sample = "attack_move", rng.choice(train["attack_move"])
        elif phase == 2:
            kind, sample = "right_click", rng.choice(hard_right_click)
        else:
            kind = rng.choices(other_kinds, weights=other_weights)[0]
            sample = rng.choice(train[kind])
        state, entities, geometry, own, positive, _, _ = on_device(sample, device)
        label = (1 if kind == "attack" else 2 if kind == "attack_move" else 0)
        scores = head(state, entities, geometry, own)
        loss = torch.logsumexp(scores[own].flatten(), 0) - torch.logsumexp(
            scores[positive, label], 0)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite balanced geometry combat loss")
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
                attack = row["by_kind"]["attack"].get("combat_kind_correct", 0)
                attack_move = row["by_kind"]["attack_move"].get(
                    "combat_kind_correct", 0)
                compact[threshold] = dict(
                    attack=attack, attack_move=attack_move,
                    combat_pair_correct=counts.get("combat_pair_correct", 0),
                    false_combat=counts.get("false_combat", 0),
                    kind_correct=counts["kind_correct"],
                    pair_correct=counts["pair_correct"])
                if (attack > 0 and attack_move > 0 and
                        counts.get("combat_pair_correct", 0) > 0 and
                        counts["kind_correct"] >= base["kind_correct"] - 30 and
                        counts["pair_correct"] >= base["pair_correct"] - 30 and
                        counts.get("false_combat", 0) <= 30):
                    score = counts["combat_pair_correct"] * 4 + counts["pair_correct"]
                    if score > best_score:
                        best_score, best_step, best_threshold = score, step, float(threshold)
                        best_state = {name: value.detach().cpu().clone()
                                      for name, value in head.state_dict().items()}
            print(json.dumps(dict(step=step, baseline=base, thresholds=compact)), flush=True)
    source = Path(__file__).with_name("whole_game_multislot_combat_geometry_probe.py")
    report = dict(
        schema="protodd-whole-game-combat-geometry-balanced-v1",
        promotion_eligible=False,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        feature_source_sha256=hashlib.sha256(source.read_bytes()).hexdigest(),
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
