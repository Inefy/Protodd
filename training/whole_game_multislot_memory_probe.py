"""Measure a periodic memory reset on the disjoint six-slot causal audit cohort.

This is a diagnostic report, not a promotion audit: the deployed runtime does
not yet use this memory policy.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_multislot_audit import score_sequence, summarize
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def probe(checkpoint, train_release, validation_release, output, *,
          games_per_matchup=8, seed=42, device="cuda", reset_interval=8):
    checkpoint, train_release, validation_release, output = map(
        Path, (checkpoint, train_release, validation_release, output))
    if output.exists() or games_per_matchup < 1 or reset_interval < 1:
        raise ValueError("existing output or invalid probe limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    train_sha, validation_sha = validate_release_pair(
        train_release, validation_release)
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if (saved.get("schema") != "protodd-whole-game-multislot-fit-v1" or
            saved.get("source_identity_sha256") != train_sha):
        raise ValueError("teacher belongs to another training release")
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    identity = json.loads((validation_release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "validation" and record["matchup"] in MATCHUPS and
                (validation_release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "validation", games_per_matchup, seed)
    rows = []
    games = Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                validation_release, "validation", game_ids=chosen):
            matchup = record["matchup"]
            games[matchup] += 1
            terrain = static_grid(load_terrain(directory))
            memory = None
            for sequence_index, sequence in enumerate(
                    cadence_sequences(trajectory_shard(directory))):
                if sequence_index % reset_interval == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                prediction = model.forward_slots(
                    {name: value.to(device) for name, value in batch.items()},
                    memory)
                memory = prediction["backbone"]["memory"]
                row = score_sequence(sequence, prediction["slots"], ids,
                                     terrain.shape[1], terrain.shape[0])
                row.update(game_id=record["game_id"], matchup=matchup)
                row["predicted_kind_indices"] = [
                    int(slot["chosen_kind"][0]) for slot in prediction["slots"]
                    [:row["predicted_commands"]]]
                rows.append(row)
    if any(games[matchup] != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("validation cohort incomplete")
    report = dict(
        schema="protodd-whole-game-multislot-memory-probe-v1",
        promotion_eligible=False,
        memory_reset_interval=reset_interval,
        teacher_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        probe_source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        training_identity_sha256=train_sha,
        validation_identity_sha256=validation_sha,
        device=device, games=dict(games),
        overall=summarize(rows),
        by_matchup={matchup: summarize([
            row for row in rows if row["matchup"] == matchup])
            for matchup in MATCHUPS}, rows=rows)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                         encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "train_release", "validation_release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=8)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--reset-interval", type=int, default=8)
    args = parser.parse_args()
    report = probe(args.checkpoint, args.train_release, args.validation_release,
                   args.output, games_per_matchup=args.games_per_matchup,
                   seed=args.seed, device=args.device,
                   reset_interval=args.reset_interval)
    print(json.dumps(dict(output=str(args.output.resolve()),
                          overall=report["overall"]), indent=2))


if __name__ == "__main__":
    main()
