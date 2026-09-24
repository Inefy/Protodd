"""Diagnose first-slot kind predictions with a replay-known actor.

The replay actor is supplied only to this offline diagnostic. The report is
not a causal audit and cannot be used as promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def probe(checkpoint, release, causal_report, output, *, device="cuda",
          reset_interval=8):
    checkpoint, release, causal_report, output = map(
        Path, (checkpoint, release, causal_report, output))
    if output.exists() or reset_interval < 1:
        raise ValueError("existing output or invalid reset interval")
    prior = json.loads(causal_report.read_text(encoding="utf8"))
    if prior.get("memory_reset_interval") != reset_interval:
        raise ValueError("causal reference has another memory policy")
    chosen = {}
    for row in prior["rows"]:
        chosen.setdefault(row["matchup"], row["game_id"])
    frame_rows = {(row["game_id"], row["frame"]): row for row in prior["rows"]
                  if chosen[row["matchup"]] == row["game_id"]}
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    counts = defaultdict(Counter)
    games = Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                release, "validation", game_ids=set(chosen.values())):
            matchup = record["matchup"]
            games[matchup] += 1
            terrain = static_grid(load_terrain(directory))
            memory = None
            for sequence_index, sequence in enumerate(
                    cadence_sequences(trajectory_shard(directory))):
                if sequence_index % reset_interval == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                batch = {name: value.to(device) for name, value in batch.items()}
                labels = sequence["labels"]
                if not labels or not sequence["actor_available"][0]:
                    memory = model(batch, memory)["memory"]
                    continue
                label = labels[0]
                actors = sorted(set(label["actor_positive"]) & ids.keys())
                if not actors:
                    memory = model(batch, memory)["memory"]
                    continue
                actor = ids[actors[0]]
                kind = label["actions"]["kind"]
                token = dict(
                    actor=torch.tensor([actor], device=device),
                    kind=torch.tensor([KINDS.index(kind)], device=device),
                    target_mode=torch.tensor([0], device=device),
                    target_entity=torch.tensor([-1], device=device),
                    delay=torch.tensor([0], device=device),
                    position=torch.zeros((1, 2), device=device),
                    has_position=torch.tensor([False], device=device),
                    unit_type=torch.tensor([0], device=device))
                decoded = model.forward_slots(batch, memory,
                                              teacher_tokens=[token], slots=1)
                memory = decoded["backbone"]["memory"]
                oracle_kind = KINDS[int(decoded["slots"][0]["kind"].argmax(-1)[0])]
                row = frame_rows[(record["game_id"],
                                  sequence["observation"]["frame"])]
                bucket = counts[kind]
                bucket["samples"] += 1
                bucket["oracle_actor_kind_correct"] += oracle_kind == kind
                bucket["free_actor_correct"] += bool(
                    row["command_slots"][0]["actor_correct"])
                bucket["free_kind_correct"] += bool(
                    row["command_slots"][0]["kind_correct"])
                bucket["predicted_action"] += row["predicted_action"]
    if len(games) != 3 or any(count != 1 for count in games.values()):
        raise ValueError("expected one disjoint validation game per matchup")
    report = dict(
        schema="protodd-whole-game-multislot-actor-probe-v1",
        promotion_eligible=False, games=dict(games),
        game_ids=chosen, memory_reset_interval=reset_interval,
        checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        causal_report_sha256=hashlib.sha256(causal_report.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        by_true_kind={kind: dict(count) for kind, count in sorted(counts.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "causal_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    result = probe(args.checkpoint, args.release, args.causal_report,
                   args.output, device=args.device)
    print(json.dumps(result["by_true_kind"], indent=2))
