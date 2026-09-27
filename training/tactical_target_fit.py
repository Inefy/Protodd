"""Fit the first bounded tactical scope: attack-unit target ranking.

Only confirmed Protoss combat-unit attack commands in verified *train* replay
shards become labels. Development games are disjoint games from the same train
split. This is imitation evidence, not a claim of better match outcomes.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import math
from pathlib import Path
import struct

import numpy as np

from .whole_game_release import key, verified_receipt


MAGIC = b"PTTACT1\0"
FEATURE_COUNT = 5
TYPE_COUNT = 256
COMBAT_TYPES = frozenset((60, 61, 65, 66, 68, 70, 71, 72, 83))


def features(actor: dict, target: dict, candidates: list[dict]) -> np.ndarray:
    ax, ay = actor["position"]
    tx, ty = target["position"]
    distance = min(1280.0, math.hypot(tx - ax, ty - ay)) / 640.0
    neighbours = sum(other["id"] != target["id"] and
                     (other["position"][0] - tx) ** 2 +
                     (other["position"][1] - ty) ** 2 <= 96 ** 2
                     for other in candidates)
    return np.asarray((distance, math.log1p(max(0, target["hp"])) / 8,
                       math.log1p(max(0, target["shields"])) / 8,
                       float(actor["own_state"]["order_target"] == target["id"]),
                       min(8, neighbours) / 8), dtype=np.float32)


def examples_from_shard(directory: Path, limit: int,
                        include_enemy_right_click: bool = False) -> list[tuple[np.ndarray, np.ndarray, int]]:
    labels = {}
    with gzip.open(directory / "imitation-labels.jsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            label = json.loads(line)
            action = label["actions"]
            kinds = ("attack", "right_click") if include_enemy_right_click else ("attack",)
            if (label["domain"] == "unit_control" and action["kind"] in kinds and
                    action["target_mode"] == "entity" and label["loss_masks"]["target_entity"] and
                    len(label["actor_positive"]) == 1):
                labels[label["observation_sequence"]] = label
    result = []
    if not labels:
        return result
    with gzip.open(directory / "observations.jsonl.gz", "rt", encoding="utf-8") as stream:
        for line in stream:
            row = json.loads(line)
            label = labels.get(row["sequence"])
            if label is None:
                continue
            if row["reason"] != "before_command" or row["frame"] != label["frame"]:
                raise ValueError("tactical label is not bound to its causal observation")
            entities = {entity["id"]: entity for entity in row["entities"]}
            actor = entities.get(label["actor_positive"][0])
            target = entities.get(label["actions"]["target_entity"])
            if (actor is None or target is None or actor["relation"] != 0 or
                    actor["type"] not in COMBAT_TYPES or target["relation"] != 1 or
                    not target["visible"] or target["hp"] <= 0):
                continue
            ax, ay = actor["position"]
            candidates = [entity for entity in row["entities"]
                          if entity["relation"] == 1 and entity["visible"] and
                          entity["hp"] > 0 and 0 <= entity["type"] < TYPE_COUNT and
                          math.hypot(entity["position"][0] - ax,
                                     entity["position"][1] - ay) <= 640]
            candidates.sort(key=lambda entity: (
                (entity["position"][0] - ax) ** 2 +
                (entity["position"][1] - ay) ** 2, entity["id"]))
            candidates = candidates[:32]
            ids = [entity["id"] for entity in candidates]
            if len(ids) < 2 or target["id"] not in ids:
                continue
            result.append((np.stack([features(actor, entity, candidates) for entity in candidates]),
                           np.asarray([entity["type"] for entity in candidates], dtype=np.int64),
                           ids.index(target["id"])))
            if len(result) >= limit:
                break
    return result


def evaluate(examples, weights, type_bias):
    if not examples:
        raise ValueError("no tactical examples")
    correct = 0
    nearest = 0
    loss = 0.0
    for values, types, label in examples:
        logits = values @ weights + type_bias[types]
        shifted = logits - logits.max()
        loss += float(np.log(np.exp(shifted).sum()) - shifted[label])
        correct += int(np.argmax(logits) == label)
        nearest += int(np.argmin(values[:, 0]) == label)
    n = len(examples)
    return {"examples": n, "top1": correct / n, "nearest_top1": nearest / n,
            "cross_entropy": loss / n}


def fit(train, dev, epochs=80, use_type_bias=True):
    weights = np.zeros(FEATURE_COUNT, dtype=np.float64)
    bias = np.zeros(TYPE_COUNT, dtype=np.float64)
    best = None
    for epoch in range(epochs):
        gradient = np.zeros_like(weights)
        bias_gradient = np.zeros_like(bias)
        for values, types, label in train:
            logits = values @ weights + bias[types]
            probability = np.exp(logits - logits.max())
            probability /= probability.sum()
            probability[label] -= 1.0
            gradient += probability @ values
            np.add.at(bias_gradient, types, probability)
        scale = 1 / len(train)
        weights -= 0.35 * (gradient * scale + 0.01 * weights)
        if use_type_bias:
            bias -= 0.35 * (bias_gradient * scale + 0.03 * bias)
        score = evaluate(dev, weights, bias)
        if best is None or score["cross_entropy"] < best[0]:
            best = (score["cross_entropy"], epoch + 1, weights.copy(), bias.copy())
    assert best is not None
    return best


def run(release: Path, output: Path, train_per_matchup: int, dev_per_matchup: int,
        per_game_limit: int, include_enemy_right_click: bool = False,
        use_type_bias: bool = True):
    if min(train_per_matchup, dev_per_matchup, per_game_limit) < 1:
        raise ValueError("positive bounded sample sizes required")
    identity_bytes = (release / "identity.json").read_bytes()
    identity = json.loads(identity_bytes)
    identity_sha = hashlib.sha256((json.dumps(identity, sort_keys=True,
        separators=(",", ":")) + "\n").encode()).hexdigest()
    train, dev = [], []
    selected = {"train": [], "development": []}
    for matchup in ("PvT", "PvZ", "PvP"):
        records = [record for record in identity["selected"]
                   if record["split"] == "train" and record["matchup"] == matchup]
        records.sort(key=lambda record: hashlib.sha256(record["game_id"].encode()).hexdigest())
        needed = train_per_matchup + dev_per_matchup
        if len(records) < needed:
            raise ValueError(f"too few train games for {matchup}")
        for index, record in enumerate(records[:needed]):
            directory = release / "games" / key(record)
            verified_receipt(directory, record, identity_sha)
            examples = examples_from_shard(directory, per_game_limit, include_enemy_right_click)
            if index < train_per_matchup:
                train.extend(examples)
                selected["train"].append((record["game_id"], matchup, len(examples)))
            else:
                dev.extend(examples)
                selected["development"].append((record["game_id"], matchup, len(examples)))
        print(f"{matchup}: {sum(item[2] for item in selected['train'] if item[1] == matchup)} "
              f"fit and {sum(item[2] for item in selected['development'] if item[1] == matchup)} "
              "development examples", flush=True)
    if len(train) < 100 or len(dev) < 30:
        raise ValueError(f"insufficient nontrivial tactical examples: {len(train)} fit, {len(dev)} development")
    _, best_epoch, weights, bias = fit(train, dev, use_type_bias=use_type_bias)
    output.mkdir(parents=True, exist_ok=True)
    model = output / "TacticalTarget-weights.bin"
    payload = struct.pack("<8sII", MAGIC, 1, FEATURE_COUNT)
    payload += struct.pack("<" + "f" * FEATURE_COUNT, *weights)
    payload += struct.pack("<" + "f" * TYPE_COUNT, *bias)
    model.write_bytes(payload)
    report = {"schema": "protodd-tactical-target-pilot-v1",
              "source_release": str(release), "identity_sha256": identity_sha,
              "trainer_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "scope": "confirmed single-combat-actor enemy target among visible enemies within 640px",
              "command_kinds": ["attack", "right_click"] if include_enemy_right_click else ["attack"],
              "type_bias": use_type_bias,
              "selection": selected, "best_epoch": best_epoch,
              "train": evaluate(train, weights, bias),
              "development": evaluate(dev, weights, bias),
              "model_sha256": hashlib.sha256(payload).hexdigest(),
              "control_qualified": False,
              "limitation": "human command imitation only; no legal-target parity, live feedback, or match-strength proof"}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--train-per-matchup", type=int, default=12)
    parser.add_argument("--dev-per-matchup", type=int, default=4)
    parser.add_argument("--per-game-limit", type=int, default=80)
    parser.add_argument("--include-enemy-right-click", action="store_true")
    parser.add_argument("--no-type-bias", action="store_true")
    args = parser.parse_args()
    print(json.dumps(run(args.release, args.output, args.train_per_matchup,
                         args.dev_per_matchup, args.per_game_limit,
                         args.include_enemy_right_click, not args.no_type_bias), indent=2))
