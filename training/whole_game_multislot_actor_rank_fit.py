"""Bounded actor-ranking fine-tune on train games omitted from the full fit.

The frozen backbone is run causally with an eight-decision memory reset. Only
the first-slot actor key and query learn a listwise ranking objective. A
separate omitted-train cohort checks whether the ranking actually improves.
This experiment is not a promotion fit.
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
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def cohorts(release, spec, *, train_games_per_matchup=5):
    trained = {game_id for group in spec["groups"]
               for ids in group.values() for game_id in ids}
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in sorted(identity["selected"], key=lambda row: row["game_id"]):
        if (record["split"] == "train" and record["game_id"] not in trained and
                (release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record["game_id"])
    if any(len(ids) < train_games_per_matchup + 1 for ids in eligible.values()) or \
            set(eligible) != {"PvP", "PvT", "PvZ"}:
        raise ValueError("insufficient omitted train games")
    dev = {matchup: ids[0] for matchup, ids in eligible.items()}
    train = {matchup: ids[1:train_games_per_matchup + 1]
             for matchup, ids in eligible.items()}
    return train, dev


def collect_features(model, release, game_ids, *, device, reset_interval=8,
                     per_kind_limit=None, seed=42):
    selected = {game_id for ids in game_ids.values() for game_id in ids}
    rng = random.Random(seed)
    buckets = defaultdict(list)
    seen = Counter()
    games = Counter()
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
                positive_ids = set(label["actor_positive"]) & ids.keys()
                if not positive_ids:
                    continue
                entities = model._entities(batch)[0]
                own = (batch["entity_mask"] & (batch["relation"] == 0))[0]
                positive = torch.zeros_like(own)
                positive[list(ids[token] for token in positive_ids)] = True
                if not (positive & own).any():
                    raise ValueError("confirmed actor is not owned")
                kind = label["actions"]["kind"]
                sample = (memory[0].detach().cpu().half(),
                          entities.detach().cpu().half(),
                          own.detach().cpu(), positive.detach().cpu())
                seen[kind] += 1
                bucket = buckets[kind]
                if per_kind_limit is None or len(bucket) < per_kind_limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < per_kind_limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(game_ids[matchup]) for matchup in game_ids):
        raise ValueError("feature cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               retained={kind: len(items)
                                         for kind, items in buckets.items()})


def logits_for(model, sample, device):
    state, entities, own, positive = sample
    state = state.to(device=device, dtype=torch.float32)[None]
    entities = entities.to(device=device, dtype=torch.float32)
    own = own.to(device=device)
    positive = positive.to(device=device)
    logits = (model.slot_actor_key(entities) *
              model.slot_actor_query(state)[0]).sum(-1) / math.sqrt(model.width)
    logits = logits.masked_fill(~own, -torch.inf)
    return logits, positive


def evaluate(model, buckets, device):
    overall = Counter()
    by_kind = {}
    with torch.inference_mode():
        for kind, samples in sorted(buckets.items()):
            counts = Counter()
            for sample in samples:
                logits, positive = logits_for(model, sample, device)
                counts["samples"] += 1
                counts["actor_correct"] += bool(positive[int(logits.argmax())])
                counts["positive_mass_loss_milli"] += round(1000 * float(
                    torch.logsumexp(logits, 0) -
                    torch.logsumexp(logits.masked_fill(~positive, -torch.inf), 0)))
            overall.update(counts)
            by_kind[kind] = dict(counts)
    return dict(overall), by_kind


def fit(checkpoint, release, output, *, device="cuda", seed=42,
        steps=400, train_games_per_matchup=5, per_kind_limit=40):
    checkpoint, release, output = map(Path, (checkpoint, release, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    train_ids, dev_ids = cohorts(release, spec,
                                 train_games_per_matchup=train_games_per_matchup)
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
        model, release, train_ids, device=device,
        per_kind_limit=per_kind_limit, seed=seed)
    dev, dev_info = collect_features(model, release,
                                     {race: [game_id] for race, game_id in dev_ids.items()},
                                     device=device, seed=seed)
    for parameter in (*model.slot_actor_key.parameters(),
                      *model.slot_actor_query.parameters()):
        parameter.requires_grad_(True)
    optimizer = torch.optim.AdamW(
        [*model.slot_actor_key.parameters(), *model.slot_actor_query.parameters()],
        lr=1e-4)
    checkpoints = []
    baseline, baseline_by_kind = evaluate(model, dev, device)
    checkpoints.append(dict(step=0, overall=baseline, by_kind=baseline_by_kind))
    best_accuracy = baseline["actor_correct"]
    best_step = 0
    best_state = None
    kinds = sorted(train)
    if not kinds or not dev:
        raise ValueError("no first-command actor examples")
    for step in range(1, steps + 1):
        kind = rng.choice(kinds)
        sample = rng.choice(train[kind])
        logits, positive = logits_for(model, sample, device)
        loss = torch.logsumexp(logits, 0) - torch.logsumexp(
            logits.masked_fill(~positive, -torch.inf), 0)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite listwise actor loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(
            [*model.slot_actor_key.parameters(), *model.slot_actor_query.parameters()], 1.0)
        optimizer.step()
        if step % 100 == 0 or step == steps:
            overall, by_kind = evaluate(model, dev, device)
            checkpoints.append(dict(step=step, overall=overall, by_kind=by_kind))
            print(json.dumps(dict(step=step, actor_correct=overall["actor_correct"],
                                  samples=overall["samples"])), flush=True)
            if overall["actor_correct"] > best_accuracy:
                best_accuracy = overall["actor_correct"]
                best_step = step
                best_state = {name: tensor.detach().cpu().clone()
                              for name, tensor in model.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-multislot-actor-rank-fit-v1",
        promotion_eligible=False,
        training_identity_sha256=saved["source_identity_sha256"],
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        memory_reset_interval=8, train_game_ids=train_ids, dev_game_ids=dev_ids,
        train=train_info, dev=dev_info, steps=steps, best_step=best_step,
        checkpoints=checkpoints)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
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
    for name in ("checkpoint", "release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--steps", type=int, default=400)
    args = parser.parse_args()
    result = fit(args.checkpoint, args.release, args.output,
                 device=args.device, steps=args.steps)
    print(json.dumps(dict(best_step=result["best_step"],
                          checkpoints=[dict(step=row["step"],
                                            overall=row["overall"])
                                       for row in result["checkpoints"]]), indent=2))
