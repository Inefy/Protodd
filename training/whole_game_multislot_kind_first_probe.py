"""Bounded kind-first actor decoder prototype on omitted train games.

This trains a separate diagnostic head over frozen causal features. It is not
part of the deployed runtime or eligible for promotion.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import copy
import hashlib
import json
import math
from pathlib import Path
import random

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


class KindFirstHead(nn.Module):
    def __init__(self, model):
        super().__init__()
        width = model.width
        self.width = width
        self.kind = nn.Linear(width, len(KINDS))
        self.kind_context = nn.Embedding(len(KINDS), 64)
        self.actor_key = copy.deepcopy(model.slot_actor_key)
        self.actor_query = nn.Linear(width + 64, width)
        self.register_buffer("supported", model.supported_kind.detach().clone())
        with torch.no_grad():
            self.kind.weight.copy_(model.slot_kind.weight[:, :width])
            self.kind.bias.copy_(model.slot_kind.bias)
            self.kind_context.weight.copy_(model.slot_kind_context.weight)
            self.actor_query.weight[:, :width].copy_(model.slot_actor_query.weight)
            self.actor_query.weight[:, width:].zero_()
            self.actor_query.bias.copy_(model.slot_actor_query.bias)

    def kind_scores(self, state):
        return self.kind(state).masked_fill(~self.supported, -torch.inf)

    def actor_scores(self, state, entities, own, kind):
        token = torch.cat((state, self.kind_context(kind)), dim=-1)
        query = self.actor_query(token)
        scores = (self.actor_key(entities) * query).sum(-1) / math.sqrt(self.width)
        return scores.masked_fill(~own, -torch.inf)


def collect_features(model, release, game_ids, *, device, per_kind_limit=None,
                     seed=42, reset_interval=8):
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
                positives = set(label["actor_positive"]) & ids.keys()
                if not positives:
                    continue
                kind = label["actions"]["kind"]
                kind_index = KINDS.index(kind)
                if not bool(model.supported_kind[kind_index]):
                    continue
                own = (batch["entity_mask"] & (batch["relation"] == 0))[0]
                positive = torch.zeros_like(own)
                positive[[ids[token] for token in positives]] = True
                if not (positive & own).any():
                    raise ValueError("confirmed actor is not owned")
                sample = (memory[0].detach().cpu().half(),
                          model._entities(batch)[0].detach().cpu().half(),
                          own.detach().cpu(), positive.detach().cpu(),
                          kind_index, int(model.slot_stop(memory[0]).argmax()) == 0)
                seen[kind] += 1
                bucket = buckets[kind]
                if per_kind_limit is None or len(bucket) < per_kind_limit:
                    bucket.append(sample)
                else:
                    replace = rng.randrange(seen[kind])
                    if replace < per_kind_limit:
                        bucket[replace] = sample
    if any(games[matchup] != len(game_ids[matchup]) for matchup in game_ids):
        raise ValueError("kind-first feature cohort incomplete")
    return dict(buckets), dict(games=dict(games), seen=dict(seen),
                               retained={kind: len(items)
                                         for kind, items in buckets.items()})


def on_device(sample, device):
    state, entities, own, positive, kind, active = sample
    return (state.to(device=device, dtype=torch.float32),
            entities.to(device=device, dtype=torch.float32),
            own.to(device=device), positive.to(device=device), kind, active)


def evaluate(head, buckets, device):
    total = Counter()
    by_kind = {}
    with torch.inference_mode():
        for name, samples in sorted(buckets.items()):
            counts = Counter()
            for sample in samples:
                state, entities, own, positive, true_kind, active = on_device(
                    sample, device)
                predicted_kind = int(head.kind_scores(state).argmax())
                actor = int(head.actor_scores(
                    state, entities, own,
                    torch.tensor(predicted_kind, device=device)).argmax())
                correct_kind = predicted_kind == true_kind
                correct_actor = bool(positive[actor])
                counts["samples"] += 1
                counts["predicted_action"] += active
                counts["kind_correct"] += correct_kind
                counts["actor_correct"] += correct_actor
                counts["pair_correct"] += correct_kind and correct_actor
                counts["active_pair_correct"] += active and correct_kind and correct_actor
            total.update(counts)
            by_kind[name] = dict(counts)
    return dict(total), by_kind


def fit(checkpoint, release, actor_rank_report, output, *, device="cuda",
        steps=1000, seed=42, per_kind_limit=80):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or steps < 1 or per_kind_limit < 1:
        raise ValueError("existing output or invalid experiment limits")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    if parent["best_step"] < 1 or parent["memory_reset_interval"] != 8:
        raise ValueError("actor-ranking parent is not selected")
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
    head = KindFirstHead(model).to(device).train()
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-4)
    baseline, baseline_by_kind = evaluate(head, dev, device)
    checkpoints = [dict(step=0, overall=baseline, by_kind=baseline_by_kind)]
    best_pair = baseline["pair_correct"]
    best_step = 0
    best_state = None
    kinds = sorted(train)
    natural_weights = [train_info["seen"][kind] for kind in kinds]
    for step in range(1, steps + 1):
        kind = (rng.choice(kinds) if step % 3 == 0 else
                rng.choices(kinds, weights=natural_weights)[0])
        sample = rng.choice(train[kind])
        state, entities, own, positive, true_kind, _ = on_device(sample, device)
        kind_scores = head.kind_scores(state)
        actor_scores = head.actor_scores(
            state, entities, own, torch.tensor(true_kind, device=device))
        loss = (F.cross_entropy(kind_scores[None],
                                torch.tensor([true_kind], device=device)) +
                torch.logsumexp(actor_scores, 0) -
                torch.logsumexp(actor_scores.masked_fill(~positive, -torch.inf), 0))
        if not torch.isfinite(loss):
            raise ValueError("nonfinite kind-first loss")
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        if step % 200 == 0 or step == steps:
            overall, by_kind = evaluate(head, dev, device)
            checkpoints.append(dict(step=step, overall=overall, by_kind=by_kind))
            print(json.dumps(dict(step=step, overall=overall,
                                  combat={kind: by_kind[kind]["pair_correct"]
                                          for kind in ("attack", "attack_move")})),
                  flush=True)
            if overall["pair_correct"] > best_pair:
                best_pair = overall["pair_correct"]
                best_step = step
                best_state = {name: tensor.detach().cpu().clone()
                              for name, tensor in head.state_dict().items()}
    report = dict(
        schema="protodd-whole-game-multislot-kind-first-probe-v1",
        promotion_eligible=False,
        source_identity_sha256=saved["source_identity_sha256"],
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
        torch.save(dict(schema="protodd-whole-game-kind-first-head-v1",
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
                                            overall=row["overall"])
                                       for row in result["checkpoints"]]), indent=2))
