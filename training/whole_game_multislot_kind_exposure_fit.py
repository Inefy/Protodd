"""Bounded first-slot kind fine-tune under the model's chosen actor.

The backbone and actor selector are frozen. This diagnostic corrects the
teacher-forcing mismatch where the kind head sees only the replay actor during
training but the model's actor during causal decoding.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random

import torch
from torch.nn import functional as F

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def collect_features(model, release, game_ids, *, device, per_kind_limit=None,
                     reset_interval=8, seed=42):
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
                true_actors = set(label["actor_positive"]) & ids.keys()
                if not true_actors:
                    continue
                kind = label["actions"]["kind"]
                kind_index = KINDS.index(kind)
                if not bool(model.supported_kind[kind_index]):
                    continue
                entities = model._entities(batch)[0]
                own = (batch["entity_mask"] & (batch["relation"] == 0))[0]
                state = memory[0]
                actor_scores = (model.slot_actor_key(entities) *
                                model.slot_actor_query(state)).sum(-1) / math.sqrt(model.width)
                actor_scores = actor_scores.masked_fill(~own, -torch.inf)
                actor_index = int(actor_scores.argmax())
                true_indices = {ids[token] for token in true_actors}
                true_index = min(true_indices)
                sample = (state.detach().cpu().half(),
                          entities[actor_index].detach().cpu().half(),
                          entities[true_index].detach().cpu().half(),
                          kind_index, actor_index in true_indices,
                          int(model.slot_stop(state).argmax()) == 0)
                seen[kind] += 1
                bucket = buckets[kind]
                if per_kind_limit is None or len(bucket) < per_kind_limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < per_kind_limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(game_ids[matchup]) for matchup in game_ids):
        raise ValueError("kind feature cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               retained={kind: len(items)
                                         for kind, items in buckets.items()})


def kind_logits(model, state, actor, device):
    features = torch.cat((state, actor)).to(device=device, dtype=torch.float32)
    logits = model.slot_kind(features)
    return logits.masked_fill(~model.supported_kind, -torch.inf)


def evaluate(model, buckets, device):
    overall = Counter()
    by_kind = {}
    with torch.inference_mode():
        for kind, samples in sorted(buckets.items()):
            counts = Counter()
            for state, chosen_actor, true_actor, target, actor_correct, active in samples:
                predicted = int(kind_logits(model, state, chosen_actor, device).argmax())
                oracle = int(kind_logits(model, state, true_actor, device).argmax())
                counts["samples"] += 1
                counts["actor_correct"] += actor_correct
                counts["predicted_action"] += active
                counts["kind_correct"] += predicted == target
                counts["active_kind_correct"] += active and predicted == target
                counts["pair_correct"] += actor_correct and predicted == target
                counts["oracle_actor_kind_correct"] += oracle == target
            overall.update(counts)
            by_kind[kind] = dict(counts)
    return dict(overall), by_kind


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=400, seed=42, per_kind_limit=40):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    if parent["best_step"] < 1 or parent["memory_reset_interval"] != 8:
        raise ValueError("actor ranking parent is not a selected periodic-reset fit")
    train_ids = parent["train_game_ids"]
    dev_ids = {race: [game_id] for race, game_id in parent["dev_game_ids"].items()}
    if set(game_id for ids in train_ids.values() for game_id in ids) & \
            set(game_id for ids in dev_ids.values() for game_id in ids):
        raise ValueError("train and development games overlap")
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
    train, train_info = collect_features(model, release, train_ids, device=device,
                                         per_kind_limit=per_kind_limit, seed=seed)
    dev, dev_info = collect_features(model, release, dev_ids, device=device, seed=seed)
    for parameter in model.slot_kind.parameters():
        parameter.requires_grad_(True)
    optimizer = torch.optim.AdamW(model.slot_kind.parameters(), lr=1e-4)
    baseline, baseline_by_kind = evaluate(model, dev, device)
    checkpoints = [dict(step=0, overall=baseline, by_kind=baseline_by_kind)]
    best_score = baseline["kind_correct"]
    best_step = 0
    best_state = None
    kinds = sorted(train)
    for step in range(1, steps + 1):
        kind = rng.choice(kinds)
        state, chosen_actor, true_actor, target, _, _ = rng.choice(train[kind])
        prediction = kind_logits(model, state, chosen_actor, device)
        replay_actor = kind_logits(model, state, true_actor, device)
        target_tensor = torch.tensor([target], device=device)
        loss = F.cross_entropy(prediction[None], target_tensor) + \
            0.5 * F.cross_entropy(replay_actor[None], target_tensor)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite kind exposure loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.slot_kind.parameters(), 1.0)
        optimizer.step()
        if step % 100 == 0 or step == steps:
            overall, by_kind = evaluate(model, dev, device)
            checkpoints.append(dict(step=step, overall=overall, by_kind=by_kind))
            print(json.dumps(dict(step=step, kind_correct=overall["kind_correct"],
                                  samples=overall["samples"])), flush=True)
            if overall["kind_correct"] > best_score:
                best_score = overall["kind_correct"]
                best_step = step
                best_state = {name: tensor.detach().cpu().clone()
                              for name, tensor in model.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-multislot-kind-exposure-fit-v1",
        promotion_eligible=False,
        source_identity_sha256=saved["source_identity_sha256"],
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=train_ids, dev_game_ids=dev_ids,
        train=train_info, dev=dev_info, steps=steps, best_step=best_step,
        checkpoints=checkpoints)
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
