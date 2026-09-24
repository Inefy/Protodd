"""Compare current actor order position with first-slot replay position.

The current order is read from the cadence observation preceding the command.
Replay actor and kind are supplied for diagnosis only; this cannot establish
causal model quality.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import statistics

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def summarize(values):
    return dict(samples=len(values), within_64px=sum(value <= 64 for value in values),
                median_error_px=statistics.median(values) if values else None)


def probe(checkpoint, release, actor_rank_report, output, *, device="cuda"):
    checkpoint, release, actor_rank_report, output = map(
        Path, (checkpoint, release, actor_rank_report, output))
    if output.exists():
        raise ValueError("existing output")
    parent = json.loads(actor_rank_report.read_text(encoding="utf8"))
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
    order_available = Counter()
    games = Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                release, "train", game_ids=set(parent["dev_game_ids"].values())):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor([terrain.shape[1] * 32, terrain.shape[0] * 32],
                                    device=device, dtype=torch.float32)
            memory = None
            for index, sequence in enumerate(cadence_sequences(trajectory_shard(directory))):
                if index % 8 == 0:
                    memory = None
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                batch = {name: value.to(device) for name, value in batch.items()}
                memory = model(batch, memory)["memory"]
                if not sequence["labels"] or not sequence["actor_available"][0]:
                    continue
                label = sequence["labels"][0]
                if not label["loss_masks"]["target_position"]:
                    continue
                actors = sorted(set(label["actor_positive"]) & ids.keys())
                if not actors:
                    continue
                kind = label["actions"]["kind"]
                actor_id = actors[0]
                actor_index = ids[actor_id]
                entity = next(entity for entity in sequence["observation"]["entities"]
                              if entity["id"] == actor_id)
                current_position = torch.tensor(entity["position"], device=device)
                order_xy = entity["own_state"]["order_position"]
                valid_order = min(order_xy) >= 0
                order = torch.tensor(order_xy, device=device, dtype=torch.float32)
                actor = model._entities(batch)[0, actor_index]
                kind_token = model.slot_kind_context(
                    torch.tensor(KINDS.index(kind), device=device))
                argument = torch.cat((memory[0], actor, kind_token))
                params = model.slot_position(argument).reshape(model.mixture_components, 5)
                mixture = params[int(params[:, 0].argmax()), 1:3].sigmoid() * map_size
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target = torch.tensor(label["actions"]["target_position"],
                                      device=device, dtype=torch.float32) * factor
                order_available[kind] += valid_order
                agreement = (valid_order and
                             float(torch.linalg.vector_norm(order - mixture)) <= 256)
                candidates = dict(mixture=mixture, actor_position=current_position,
                                  order_if_available=order if valid_order else mixture,
                                  order_if_agrees=order if agreement else mixture)
                if valid_order:
                    candidates["order_only"] = order
                for name, position in candidates.items():
                    error = float(torch.linalg.vector_norm(position - target))
                    errors[name].append(error)
                    by_kind[kind][name].append(error)
    if any(games[matchup] != 1 for matchup in parent["dev_game_ids"]):
        raise ValueError("position-order development cohort incomplete")
    report = dict(
        schema="protodd-whole-game-position-order-probe-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        actor_rank_report_sha256=hashlib.sha256(actor_rank_report.read_bytes()).hexdigest(),
        memory_reset_interval=8, dev_game_ids=parent["dev_game_ids"],
        games=dict(games), order_available=dict(order_available),
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
    result = probe(args.checkpoint, args.release, args.actor_rank_report,
                   args.output, device=args.device)
    print(json.dumps(dict(overall=result["overall"],
                          order_available=result["order_available"]), indent=2))
