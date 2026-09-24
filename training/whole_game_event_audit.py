"""Causal full-trajectory audit of whole-game action timing.

Each validation game is replayed at its natural 24-frame cadence with persistent
model memory. The final censored window updates memory but has no timing label.
This measures the tournament policy's event head rather than balanced examples.
"""
from __future__ import annotations

import argparse
from collections import defaultdict
import hashlib
import json
import math
from pathlib import Path

import torch

from .whole_game_conditional_model import ConditionalWholeGameModel
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_model import WholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


SCHEMA = "protodd-whole-game-event-audit-v1"
THRESHOLDS = (0.25, 0.5, 0.75, 0.9)


def summarize(rows):
    if not rows:
        raise ValueError("event audit needs labeled cadence windows")
    positives = sum(row["event"] for row in rows)
    ranked = sorted(rows, key=lambda row: (-row["probability"], row["game_id"], row["frame"]))
    found = 0
    precision_at_hits = 0.0
    for rank, row in enumerate(ranked, start=1):
        if row["event"]:
            found += 1
            precision_at_hits += found / rank
    scores = {}
    for threshold in THRESHOLDS:
        predicted = [row for row in rows if row["probability"] >= threshold]
        true_positive = sum(row["event"] for row in predicted)
        scores[str(threshold)] = dict(predicted=len(predicted), true_positive=true_positive,
                                      precision=true_positive / len(predicted) if predicted else None,
                                      recall=true_positive / positives if positives else None)
    brier = sum((row["probability"] - row["event"]) ** 2 for row in rows) / len(rows)
    rate = positives / len(rows)
    average_precision = precision_at_hits / positives if positives else None
    return dict(windows=len(rows), events=positives, event_rate=rate,
                mean_predicted_probability=sum(row["probability"] for row in rows) / len(rows),
                average_precision=average_precision,
                average_precision_above_prior=average_precision - rate
                if average_precision is not None else None,
                brier=brier, constant_prior_brier=rate * (1 - rate),
                thresholds=scores)


def audit(checkpoint, train_release, validation_release, output, *, games_per_matchup=1,
          seed=42, device="cpu"):
    checkpoint, train_release, validation_release, output = map(
        Path, (checkpoint, train_release, validation_release, output))
    if output.exists():
        raise FileExistsError(output)
    if games_per_matchup < 1 or device not in ("cpu", "cuda"):
        raise ValueError("invalid event audit limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    train_sha, validation_sha = validate_release_pair(train_release, validation_release)
    source = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if source.get("source_identity_sha256") != train_sha:
        raise ValueError("teacher does not belong to the training release")
    state = source["state_dict"]
    width = state["memory.weight_hh"].shape[1]
    components = state["position.weight"].shape[0] // 5
    conditional = "conditional_kind.weight" in state
    model = (ConditionalWholeGameModel if conditional else WholeGameModel)(
        width=width, mixture_components=components)
    model.load_state_dict(state, strict=True)
    torch.set_num_threads(2)
    model.to(device).eval()
    identity = json.loads((validation_release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "validation" and record["matchup"] in MATCHUPS and
                (validation_release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "validation", games_per_matchup, seed)
    rows = []
    games = defaultdict(int)
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                validation_release, "validation", game_ids=chosen):
            matchup = record["matchup"]
            games[matchup] += 1
            terrain = static_grid(load_terrain(directory))
            memory = None
            for observation, target in trajectory_shard(directory):
                if not target["update_memory"]:
                    continue
                encoded, _, _ = encode_observation(observation, terrain)
                output_row = model({name: value.to(device) for name, value in encoded.items()},
                                   memory)
                memory = output_row["memory"]
                if target["event"] is None:
                    continue
                probability = torch.sigmoid(output_row["event"])[0].item()
                if not math.isfinite(probability):
                    raise ValueError("nonfinite event prediction")
                rows.append(dict(game_id=record["game_id"], matchup=matchup,
                                 frame=observation["frame"], event=target["event"],
                                 probability=probability))
    if any(games[matchup] != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("disjoint validation cohort incomplete")
    report = dict(schema=SCHEMA, checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                  training_identity_sha256=train_sha, validation_identity_sha256=validation_sha,
                  model_family="actor_conditional" if conditional else "baseline",
                  device=device, games=dict(games), strength_validated=False,
                  overall=summarize(rows),
                  by_matchup={matchup: summarize([row for row in rows if row["matchup"] == matchup])
                              for matchup in MATCHUPS}, rows=rows)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "train_release", "validation_release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=1)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cpu")
    args = parser.parse_args()
    report = audit(args.checkpoint, args.train_release, args.validation_release,
                   args.output, games_per_matchup=args.games_per_matchup,
                   seed=args.seed, device=args.device)
    print(json.dumps(dict(output=str(args.output.resolve()), overall=report["overall"]), indent=2))


if __name__ == "__main__":
    main()
