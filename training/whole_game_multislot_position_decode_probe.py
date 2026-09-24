"""Compare causal position-mixture decoders with replay actor/kind supplied.

Replay actor and kind are diagnostic inputs only. This cannot be used as a
causal audit or as promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics

import torch
from torch.nn import functional as F

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def probe(checkpoint, release, actor_rank_report, output, *, device="cuda",
          reset_interval=8):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists() or reset_interval < 1:
        raise ValueError("existing output or invalid reset interval")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
    dev_ids = parent["dev_game_ids"]
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    errors = defaultdict(list)
    by_kind = defaultdict(lambda: defaultdict(list))
    games = Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                release, "train", game_ids=set(dev_ids.values())):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor([terrain.shape[1] * 32,
                                     terrain.shape[0] * 32], device=device)
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
                if not label["loss_masks"]["target_position"]:
                    continue
                actors = sorted(set(label["actor_positive"]) & ids.keys())
                if not actors:
                    continue
                kind = label["actions"]["kind"]
                actor = model._entities(batch)[0, ids[actors[0]]]
                kind_token = model.slot_kind_context(
                    torch.tensor(KINDS.index(kind), device=device))
                features = torch.cat((memory[0], actor, kind_token))
                parameters = model.slot_position(features).reshape(
                    model.mixture_components, 5)
                weights = F.softmax(parameters[:, 0], dim=-1)
                means = parameters[:, 1:3].sigmoid()
                scales = F.softplus(parameters[:, 3:5]).clamp(min=0.01, max=1.0)
                highest_weight = means[int(weights.argmax())]
                highest_peak = means[int((weights.log() - scales.log().sum(-1)).argmax())]
                expectation = (weights[:, None] * means).sum(0)
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target = torch.tensor(label["actions"]["target_position"],
                                      device=device, dtype=torch.float32) * factor
                for name, prediction in (
                        ("highest_weight", highest_weight),
                        ("highest_peak", highest_peak),
                        ("weighted_mean", expectation)):
                    error = float(torch.linalg.vector_norm(prediction * map_size - target))
                    errors[name].append(error)
                    by_kind[kind][name].append(error)
    if len(games) != 3 or any(count != 1 for count in games.values()):
        raise ValueError("position development cohort incomplete")

    def summarize(values):
        return dict(samples=len(values), within_64px=sum(value <= 64 for value in values),
                    median_error_px=statistics.median(values))

    report = dict(
        schema="protodd-whole-game-multislot-position-decode-probe-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        games=dict(games), game_ids=dev_ids, memory_reset_interval=reset_interval,
        checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        overall={name: summarize(values) for name, values in errors.items()},
        by_kind={kind: {name: summarize(values) for name, values in modes.items()}
                 for kind, modes in sorted(by_kind.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "actor_rank_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    print(json.dumps(probe(args.checkpoint, args.release,
                           args.actor_rank_report, args.output,
                           device=args.device)["overall"], indent=2))
