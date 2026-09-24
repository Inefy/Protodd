"""Balance anchor-positive examples in the bounded position-pointer probe."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import random

import torch
from torch.nn import functional as F

from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_multislot_position_pointer_probe import (
    PositionPointer, collect_features, evaluate, on_device)


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=2000, seed=42, per_kind_limit=250):
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
    positives = {kind: [sample for sample in samples if sample[-1] >= 0]
                 for kind, samples in train.items()}
    positives = {kind: samples for kind, samples in positives.items() if samples}
    negatives = [sample for samples in train.values() for sample in samples
                 if sample[-1] < 0]
    if not positives or not negatives:
        raise ValueError("both anchor classes are required")
    positive_kinds = sorted(positives)
    weights = [train_info["seen"][kind] for kind in positive_kinds]
    head = PositionPointer(model).to(device).train()
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(head, dev, device))]
    baseline_hits = checkpoints[0]["metrics"]["modes"]["mixture"]["within_64px"]
    best_hits = baseline_hits
    best_step = 0
    best_state = None
    for step in range(1, steps + 1):
        kind = (rng.choice(positive_kinds) if step % 4 == 0 else
                rng.choices(positive_kinds, weights=weights)[0])
        positive = rng.choice(positives[kind])
        negative = rng.choice(negatives)
        pos_argument, pos_entities, pos_visible, _, _, _, nearest = on_device(
            positive, device)
        neg_argument, neg_entities, neg_visible, _, _, _, _ = on_device(
            negative, device)
        pos_logits, pos_gate = head(pos_argument, pos_entities, pos_visible)
        _, neg_gate = head(neg_argument, neg_entities, neg_visible)
        loss = (F.cross_entropy(
                    pos_logits[None], torch.tensor([nearest], device=device)) +
                0.5 * F.softplus(-pos_gate) + 0.5 * F.softplus(neg_gate))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite balanced position-pointer loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 500 == 0 or step == steps:
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
        schema="protodd-whole-game-multislot-position-pointer-balanced-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        feature_source_sha256=hashlib.sha256(
            Path(__file__).with_name("whole_game_multislot_position_pointer_probe.py")
            .read_bytes()).hexdigest(),
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
    parser.add_argument("--steps", type=int, default=2000)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.actor_rank_report,
                 args.output, device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          checkpoints=[dict(step=row["step"],
                                            modes=row["metrics"]["modes"])
                                       for row in result["checkpoints"]]), indent=2))
