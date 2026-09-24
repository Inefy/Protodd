"""Disjoint validation of the fixed causal order-position fallback rule.

The replay actor and kind are supplied only to isolate position quality. The
validation games are the same 8-per-matchup cohort as the six-slot audit, but
this is not a complete causal command audit or promotion evidence.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_model import KINDS
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_position_order_probe import summarize
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def probe(checkpoint, train_release, validation_release, development_report,
          output, *, device="cuda", games_per_matchup=8, seed=42):
    checkpoint, train_release, validation_release, development_report, output = map(
        Path, (checkpoint, train_release, validation_release, development_report, output))
    if output.exists() or games_per_matchup < 1:
        raise ValueError("existing output or invalid game limit")
    train_sha, validation_sha = validate_release_pair(train_release, validation_release)
    development = json.loads(development_report.read_text(encoding="utf8"))
    if development["overall"]["order_if_agrees"]["within_64px"] <= \
            development["overall"]["mixture"]["within_64px"]:
        raise ValueError("development rule was not selected")
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["source_identity_sha256"] != train_sha:
        raise ValueError("source checkpoint and release disagree")
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
    games, selected_order = Counter(), Counter()
    errors = defaultdict(list)
    by_kind = defaultdict(lambda: defaultdict(list))
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                validation_release, "validation", game_ids=chosen):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            map_size = torch.tensor([terrain.shape[1] * 32, terrain.shape[0] * 32],
                                    device=device, dtype=torch.float32)
            memory = None
            for index, sequence in enumerate(cadence_sequences(trajectory_shard(directory))):
                if index % 8 == 0:
                    memory = None
                if sequence["observation"]["reason"] != "cadence":
                    raise ValueError("noncausal position observation")
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
                actor = model._entities(batch)[0, ids[actor_id]]
                kind_token = model.slot_kind_context(
                    torch.tensor(KINDS.index(kind), device=device))
                argument = torch.cat((memory[0], actor, kind_token))
                params = model.slot_position(argument).reshape(model.mixture_components, 5)
                mixture = params[int(params[:, 0].argmax()), 1:3].sigmoid() * map_size
                observed_actor = next(entity for entity in sequence["observation"]["entities"]
                                      if entity["id"] == actor_id)
                order_xy = observed_actor["own_state"]["order_position"]
                valid_order = min(order_xy) >= 0
                order = torch.tensor(order_xy, device=device, dtype=torch.float32)
                agreement = valid_order and float(torch.linalg.vector_norm(
                    order - mixture)) <= 256
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                target = torch.tensor(label["actions"]["target_position"],
                                      device=device, dtype=torch.float32) * factor
                baseline_error = float(torch.linalg.vector_norm(mixture - target))
                order_error = float(torch.linalg.vector_norm(order - target)) if \
                    valid_order else None
                selected_error = order_error if agreement else baseline_error
                errors["mixture"].append(baseline_error)
                errors["order_if_agrees"].append(selected_error)
                by_kind[kind]["mixture"].append(baseline_error)
                by_kind[kind]["order_if_agrees"].append(selected_error)
                selected_order["samples"] += 1
                selected_order["valid"] += valid_order
                selected_order["used"] += agreement
                if agreement:
                    selected_order["better"] += order_error < baseline_error
                    selected_order["worse"] += order_error > baseline_error
    if any(games[matchup] != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("validation position cohort incomplete")
    report = dict(
        schema="protodd-whole-game-position-order-validation-v1",
        promotion_eligible=False, replay_actor_and_kind_supplied=True,
        order_agreement_radius_px=256, memory_reset_interval=8,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        development_report_sha256=hashlib.sha256(development_report.read_bytes()).hexdigest(),
        train_identity_sha256=train_sha, validation_identity_sha256=validation_sha,
        games=dict(games), selected_order=dict(selected_order),
        overall={name: summarize(values) for name, values in errors.items()},
        by_kind={kind: {name: summarize(values) for name, values in modes.items()}
                 for kind, modes in sorted(by_kind.items())})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "train_release", "validation_release",
                 "development_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    result = probe(args.checkpoint, args.train_release, args.validation_release,
                   args.development_report, args.output, device=args.device)
    print(json.dumps(dict(overall=result["overall"],
                          selected_order=result["selected_order"]), indent=2))
