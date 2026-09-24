"""Bounded joint actor/combat classifier over a frozen whole-game encoder.

An independent three-class actor score detects attack and attack-move. A
conservative margin may override the source decoder; all other commands fall
back to the source. This is omitted-train research, not promotion evidence.
"""
from __future__ import annotations

from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import random

import torch
from torch import nn

from .whole_game_model import KINDS
from .whole_game_multislot_kind_first_probe import collect_features, on_device
from .whole_game_multislot_model import MultiSlotWholeGameModel


COMBAT = ("attack", "attack_move")
THRESHOLDS = (0.0, 1.0, 2.0, 3.0, 4.0)


class CombatPairHead(nn.Module):
    def __init__(self, width):
        super().__init__()
        self.net = nn.Sequential(nn.Linear(width * 2, 192), nn.GELU(),
                                 nn.Linear(192, 3))

    def forward(self, state, entities, own):
        state = state.expand(entities.shape[0], -1)
        logits = self.net(torch.cat((state, entities), -1))
        return logits.masked_fill(~own[:, None], -torch.inf)


def source_prediction(model, state, entities, own):
    actor_scores = (model.slot_actor_key(entities) *
                    model.slot_actor_query(state)).sum(-1) / math.sqrt(model.width)
    actor_index = int(actor_scores.masked_fill(~own, -torch.inf).argmax())
    scores = model.slot_kind(torch.cat((state, entities[actor_index])))
    kind_index = int(scores.masked_fill(~model.supported_kind, -torch.inf).argmax())
    return actor_index, kind_index


def evaluate(model, head, buckets, device):
    results = {str(threshold): Counter() for threshold in THRESHOLDS}
    baseline = Counter()
    by_kind = {str(threshold): {} for threshold in THRESHOLDS}
    with torch.inference_mode():
        for name, samples in sorted(buckets.items()):
            local = {str(threshold): Counter() for threshold in THRESHOLDS}
            base_local = Counter()
            for sample in samples:
                state, entities, own, positive, true_kind, active = on_device(sample, device)
                source_actor, source_kind = source_prediction(model, state, entities, own)
                source_pair = bool(positive[source_actor]) and source_kind == true_kind
                base_local["samples"] += 1
                base_local["kind_correct"] += source_kind == true_kind
                base_local["pair_correct"] += source_pair
                base_local["combat_kind_correct"] += (name in COMBAT and
                                                      source_kind == true_kind)
                scores = head(state, entities, own)
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
            baseline.update(base_local)
            for threshold in THRESHOLDS:
                key = str(threshold)
                results[key].update(local[key])
                by_kind[key][name] = dict(local[key])
    return dict(baseline=dict(baseline), thresholds={
        key: dict(overall=counts, by_kind=by_kind[key])
        for key, counts in results.items()})


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=1200, seed=42, per_kind_limit=120):
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
    train, train_info = collect_features(
        model, release, parent["train_game_ids"], device=device,
        per_kind_limit=per_kind_limit, seed=seed)
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    dev, dev_info = collect_features(model, release, dev_ids, device=device, seed=seed)
    head = CombatPairHead(model.width).to(device)
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    checkpoints = [dict(step=0, metrics=evaluate(model, head, dev, device))]
    base = checkpoints[0]["metrics"]["baseline"]
    best_score, best_step, best_threshold, best_state = -1, 0, None, None
    combat_kinds = [kind for kind in COMBAT if kind in train]
    other_kinds = [kind for kind in train if kind not in COMBAT]
    other_weights = [train_info["seen"][kind] for kind in other_kinds]
    combat_weights = [train_info["seen"][kind] for kind in combat_kinds]
    if not combat_kinds or not other_kinds:
        raise ValueError("combat and noncombat train examples required")
    for step in range(1, steps + 1):
        if step % 2:
            kind = rng.choices(combat_kinds, weights=combat_weights)[0]
        else:
            kind = rng.choices(other_kinds, weights=other_weights)[0]
        state, entities, own, positive, true_kind, _ = on_device(
            rng.choice(train[kind]), device)
        label = COMBAT.index(kind) + 1 if kind in COMBAT else 0
        scores = head(state, entities, own)
        numerator = torch.logsumexp(scores[positive, label], 0)
        denominator = torch.logsumexp(scores[own].flatten(), 0)
        loss = denominator - numerator
        if not torch.isfinite(loss):
            raise ValueError("nonfinite combat-pair loss")
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
    dependency = Path(__file__).with_name("whole_game_multislot_kind_first_probe.py")
    report = dict(
        schema="protodd-whole-game-combat-pair-probe-v1",
        promotion_eligible=False,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        feature_source_sha256=hashlib.sha256(dependency.read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=parent["train_game_ids"],
        dev_game_ids=parent["dev_game_ids"], train=train_info, dev=dev_info,
        steps=steps, thresholds=THRESHOLDS, best_step=best_step,
        best_threshold=best_threshold, checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    if best_state is not None:
        torch.save(dict(schema="protodd-whole-game-combat-pair-head-v1",
                        source_checkpoint_sha256=report["source_checkpoint_sha256"],
                        threshold=best_threshold, state_dict=best_state), output / "head.pt")
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
                          best_threshold=result["best_threshold"]), indent=2))
