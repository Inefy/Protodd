"""Free-running causal action audit for the six-slot GPU teacher.

No future replay command is passed into the model. Each cadence decision is
decoded autoregressively and compared with confirmed following commands.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_model import KINDS, TARGET_MODES
from .whole_game_multislot_fit import SOURCE_FILES as FIT_SOURCE_FILES
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


SCHEMA = "protodd-whole-game-multislot-audit-v1"


def score_sequence(sequence, decoded, ids, width, height):
    labels = sequence["labels"]
    reverse_ids = {index: token for token, index in ids.items()}
    predicted_count = next((index for index, output in enumerate(decoded)
                            if int(output["chosen_stop"][0]) == 1), len(decoded))
    row = dict(frame=sequence["observation"]["frame"],
               true_commands=len(labels), predicted_commands=predicted_count,
               true_action=bool(labels), predicted_action=predicted_count > 0,
               command_slots=[], early_probe_moves=[])
    if row["frame"] < 1200:
        own_types = {entity["id"]: entity["type"]
                     for entity in sequence["observation"].get("entities", [])
                     if entity["relation"] == 0}
        for output in decoded[:predicted_count]:
            actor = reverse_ids.get(int(output["chosen_actor"][0]))
            if (own_types.get(actor) != 64 or
                    KINDS[int(output["chosen_kind"][0])] not in
                    {"move", "right_click", "attack", "attack_move", "patrol"} or
                    TARGET_MODES[int(output["chosen_target_mode"][0])] != "position"):
                continue
            position = output["chosen_position"][0]
            row["early_probe_moves"].append(dict(
                actor=actor, x=round(float(position[0]) * width * 32),
                y=round(float(position[1]) * height * 32)))
    for index, label in enumerate(labels[:len(decoded)]):
        active = index < predicted_count
        actions, masks = label["actions"], label["loss_masks"]
        result = dict(active=active, kind=actions["kind"],
                      actor_known=sequence["actor_available"][index],
                      target_mode_known=bool(masks["target_mode"]),
                      target_entity_known=bool(masks["target_entity"] and
                                               sequence["target_available"][index]),
                      target_position_known=bool(masks["target_position"]),
                      unit_type_known=bool(masks["unit_type"]))
        if not active:
            result.update(kind_correct=False, actor_correct=False,
                          target_mode_correct=False, target_entity_correct=False,
                          target_position_error_px=None, unit_type_correct=False,
                          delay_error_frames=None, full_signature_correct=False)
            row["command_slots"].append(result)
            continue
        output = decoded[index]
        predicted_actor = reverse_ids.get(int(output["chosen_actor"][0]))
        predicted_target = reverse_ids.get(int(output["chosen_target_entity"][0]))
        result["kind_correct"] = KINDS[int(output["chosen_kind"][0])] == actions["kind"]
        result["actor_correct"] = (result["actor_known"] and
                                   predicted_actor in label["actor_positive"])
        result["target_mode_correct"] = (
            result["target_mode_known"] and
            TARGET_MODES[int(output["chosen_target_mode"][0])] == actions["target_mode"])
        result["target_entity_correct"] = (
            result["target_entity_known"] and
            predicted_target == actions["target_entity"])
        result["unit_type_correct"] = (
            result["unit_type_known"] and
            int(output["chosen_unit_type"][0]) == actions["unit_type"])
        result["delay_error_frames"] = abs(
            int(output["chosen_delay"][0]) - (label["frame"] - row["frame"]))
        result["target_position_error_px"] = None
        if result["target_position_known"]:
            factor = 32 if label["coordinate_space"] == "build_tile" else 1
            x, y = actions["target_position"]
            predicted = output["chosen_position"][0]
            result["target_position_error_px"] = (
                ((float(predicted[0]) * width * 32 - x * factor) ** 2 +
                 (float(predicted[1]) * height * 32 - y * factor) ** 2) ** 0.5)
        result["full_signature_correct"] = bool(
            result["kind_correct"] and result["actor_correct"] and
            (not result["target_mode_known"] or result["target_mode_correct"]) and
            (not masks["target_entity"] or result["target_entity_correct"]) and
            (not result["target_position_known"] or
             result["target_position_error_px"] <= 64) and
            (not result["unit_type_known"] or result["unit_type_correct"]) and
            result["delay_error_frames"] <= 2)
        row["command_slots"].append(result)
    return row


def early_mass_probe_moves(rows):
    """Find persistent four-Probe moves toward a single 64-pixel target cell."""
    groups = defaultdict(lambda: (set(), set()))
    for row in rows:
        if row["frame"] >= 1200:
            continue
        for move in row.get("early_probe_moves", []):
            key = (row.get("game_id"), move["x"] // 64, move["y"] // 64)
            actors, frames = groups[key]
            actors.add(move["actor"])
            frames.add(row["frame"])
    return sum(len(actors) >= 4 and len(frames) >= 3
               for actors, frames in groups.values())


def summarize(rows):
    if not rows:
        raise ValueError("empty validation action audit")
    slots = [slot for row in rows for slot in row["command_slots"]]
    first = [row["command_slots"][0] for row in rows if row["command_slots"]]
    position = [slot for slot in slots if slot["target_position_known"]]
    true_kinds = Counter(slot["kind"] for slot in slots)
    result = dict(
        windows=len(rows), action_windows=sum(row["true_action"] for row in rows),
        replay_commands=sum(row["true_commands"] for row in rows),
        predicted_commands=sum(row["predicted_commands"] for row in rows),
        first_action_correct=sum(row["true_action"] == row["predicted_action"]
                                 for row in rows),
        exact_command_count=sum(row["true_commands"] == row["predicted_commands"]
                                for row in rows),
        first_kind_correct=sum(slot["kind_correct"] for slot in first),
        first_non_right_click_correct=sum(
            slot["kind"] != "right_click" and slot["kind_correct"] for slot in first),
        first_actor_correct=sum(slot["actor_correct"] for slot in first),
        slot_commands=len(slots),
        slot_active=sum(slot["active"] for slot in slots),
        kind_correct=sum(slot["kind_correct"] for slot in slots),
        non_right_click_commands=sum(slot["kind"] != "right_click" for slot in slots),
        non_right_click_correct=sum(slot["kind"] != "right_click" and
                                    slot["kind_correct"] for slot in slots),
        kind_by_name={kind: dict(samples=count, correct=sum(
            slot["kind"] == kind and slot["kind_correct"] for slot in slots),
            full_signature_correct=sum(slot["kind"] == kind and
                                       slot["full_signature_correct"] for slot in slots))
            for kind, count in sorted(true_kinds.items())},
        actor_correct=sum(slot["actor_correct"] for slot in slots),
        target_mode_known=sum(slot["target_mode_known"] for slot in slots),
        target_mode_correct=sum(slot["target_mode_correct"] for slot in slots),
        target_entity_known=sum(slot["target_entity_known"] for slot in slots),
        target_entity_correct=sum(slot["target_entity_correct"] for slot in slots),
        position_known=len(position),
        position_within_64px=sum(slot["active"] and
                                 slot["target_position_error_px"] <= 64
                                 for slot in position),
        unit_type_known=sum(slot["unit_type_known"] for slot in slots),
        unit_type_correct=sum(slot["unit_type_correct"] for slot in slots),
        delay_within_2_frames=sum(slot["active"] and
                                  slot["delay_error_frames"] <= 2 for slot in slots),
        full_signature_correct=sum(slot["full_signature_correct"] for slot in slots),
        early_mass_probe_move_patterns=early_mass_probe_moves(rows),
        predicted_kind_distribution=dict(Counter(
            KINDS[int(output)] for row in rows for output in
            row.get("predicted_kind_indices", []))))
    errors = [slot["target_position_error_px"] for slot in position
              if slot["active"]]
    result["position_median_error_px"] = statistics.median(errors) if errors else None
    return result


def audit(checkpoint, train_release, validation_release, output, *,
          games_per_matchup=8, seed=42, device="cuda"):
    checkpoint, train_release, validation_release, output = map(
        Path, (checkpoint, train_release, validation_release, output))
    if output.exists() or games_per_matchup < 1 or device not in ("cpu", "cuda"):
        raise ValueError("existing output or invalid audit limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    train_sha, validation_sha = validate_release_pair(
        train_release, validation_release)
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if (saved.get("schema") != "protodd-whole-game-multislot-fit-v1" or
            saved.get("source_identity_sha256") != train_sha):
        raise ValueError("teacher belongs to another training release")
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    state = saved["state_dict"]
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(state, strict=True)
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
            for sequence in cadence_sequences(trajectory_shard(directory)):
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
    report = dict(schema=SCHEMA, teacher_sha256=hashlib.sha256(
                      checkpoint.read_bytes()).hexdigest(),
                  source_code_sha256={name: hashlib.sha256(
                      (Path(__file__).parent / name).read_bytes()).hexdigest()
                      for name in ("whole_game_multislot_audit.py", *FIT_SOURCE_FILES)},
                  training_identity_sha256=train_sha,
                  validation_identity_sha256=validation_sha,
                  device=device, games=dict(games), strength_validated=False,
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
    args = parser.parse_args()
    report = audit(args.checkpoint, args.train_release, args.validation_release,
                   args.output, games_per_matchup=args.games_per_matchup,
                   seed=args.seed, device=args.device)
    print(json.dumps(dict(output=str(args.output.resolve()),
                          overall=report["overall"]), indent=2))


if __name__ == "__main__":
    main()
