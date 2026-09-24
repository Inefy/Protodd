"""Measure action choices at deployable cadence observations, before commands.

The model sees only the cadence observation and prior causal memory. Confirmed
commands in the following 24 frames are scoring targets, never model inputs.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path
import statistics

import torch

from .whole_game_action_schema import select_supported_pair
from .whole_game_conditional_model import ConditionalWholeGameModel
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_model import KINDS, TARGET_MODES, WholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


SCHEMA = "protodd-whole-game-cadence-action-audit-v1"


def score_window(pending):
    event = pending["event"]
    if event is None:
        return None
    labels = pending["labels"]
    if bool(event) != bool(labels):
        raise ValueError("cadence event and confirmed following commands disagree")
    if not labels:
        return None
    first = labels[0]
    actions = first["actions"]
    kinds = [label["actions"]["kind"] for label in labels]
    actors = first["actor_positive"]
    known = bool(actors and any(actor in pending["own_ids"] for actor in actors))
    row = dict(game_id=pending["game_id"], matchup=pending["matchup"],
                frame=pending["frame"], first_action_lag=first["frame"] - pending["frame"],
                commands_in_window=len(labels), first_kind=actions["kind"],
                predicted_kind=pending["predicted_kind"],
                first_kind_correct=pending["predicted_kind"] == actions["kind"],
                any_kind_correct=pending["predicted_kind"] in kinds,
                supported_joint_kind=pending["joint_kind"],
                supported_joint_mode=pending["joint_mode"],
                joint_pair_correct=(pending["joint_kind"] == actions["kind"] and
                                    pending["joint_mode"] == actions["target_mode"]),
                actor_known=known,
                actor_correct=known and pending["actor_id"] in actors,
                event_probability=pending["event_probability"])
    if "argument_predictions" not in pending or "loss_masks" not in first:
        return row
    predicted = pending["argument_predictions"]
    masks = first["loss_masks"]
    row["target_mode_known"] = bool(masks["target_mode"])
    row["target_mode_correct"] = (row["target_mode_known"] and
                                  predicted["target_mode"] == actions["target_mode"])
    row["target_entity_known"] = bool(
        masks["target_entity"] and actions["target_entity"] in pending["visible_ids"])
    row["target_entity_correct"] = (row["target_entity_known"] and
                                    predicted["target_entity"] == actions["target_entity"])
    row["target_position_known"] = bool(masks["target_position"])
    row["target_position_error_px"] = None
    row["target_position_oracle_error_px"] = None
    if row["target_position_known"]:
        factor = 32 if first["coordinate_space"] == "build_tile" else 1
        true_x, true_y = (value * factor for value in actions["target_position"])
        errors = [((x - true_x) ** 2 + (y - true_y) ** 2) ** 0.5
                  for x, y in predicted["position_components"]]
        row["target_position_error_px"] = errors[predicted["position_component"]]
        row["target_position_oracle_error_px"] = min(errors)
    row["unit_type_known"] = bool(masks["unit_type"])
    row["unit_type_correct"] = (row["unit_type_known"] and
                                predicted["unit_type"] == actions["unit_type"])
    row["argument_known"] = {name: bool(masks[name]) for name in
                             ("queued", "order", "technology", "upgrade", "queue_slot")}
    row["argument_correct"] = {name: bool(masks[name] and predicted[name] == actions[name])
                               for name in row["argument_known"]}
    row["full_signature_known"] = bool(
        known and row["target_mode_known"] and
        (not masks["target_entity"] or row["target_entity_known"]))
    row["full_signature_correct"] = bool(
        row["full_signature_known"] and row["joint_pair_correct"] and
        row["actor_correct"] and
        (not row["target_entity_known"] or row["target_entity_correct"]) and
        (not row["target_position_known"] or row["target_position_error_px"] <= 64) and
        (not row["unit_type_known"] or row["unit_type_correct"]) and
        all(not row["argument_known"][name] or row["argument_correct"][name]
            for name in row["argument_known"]))
    return row


def summarize(rows, windows, events):
    if not rows or windows < len(rows) or events != len(rows):
        raise ValueError("incomplete cadence action rows")
    true_kinds = Counter(row["first_kind"] for row in rows)
    majority_kind, majority_count = true_kinds.most_common(1)[0]
    result = dict(windows=windows, action_windows=events, event_rate=events / windows,
                first_kind_top1=sum(row["first_kind_correct"] for row in rows),
                any_kind_top1=sum(row["any_kind_correct"] for row in rows),
                supported_joint_pair_top1=sum(row["joint_pair_correct"] for row in rows),
                actor_known=sum(row["actor_known"] for row in rows),
                actor_top1=sum(row["actor_correct"] for row in rows),
                majority_kind=majority_kind, majority_count=majority_count,
                first_action_lag_median=statistics.median(row["first_action_lag"] for row in rows),
                true_kinds=dict(true_kinds),
                predicted_kinds=dict(Counter(row["predicted_kind"] for row in rows)))
    if all("full_signature_known" in row for row in rows):
        result.update(
            target_mode_known=sum(row["target_mode_known"] for row in rows),
            target_mode_top1=sum(row["target_mode_correct"] for row in rows),
            target_entity_known=sum(row["target_entity_known"] for row in rows),
            target_entity_top1=sum(row["target_entity_correct"] for row in rows),
            target_position_known=sum(row["target_position_known"] for row in rows),
            target_position_within_64px=sum(
                row["target_position_known"] and row["target_position_error_px"] <= 64
                for row in rows),
            target_position_oracle_within_64px=sum(
                row["target_position_known"] and row["target_position_oracle_error_px"] <= 64
                for row in rows),
            unit_type_known=sum(row["unit_type_known"] for row in rows),
            unit_type_top1=sum(row["unit_type_correct"] for row in rows),
            full_signature_known=sum(row["full_signature_known"] for row in rows),
            full_signature_top1=sum(row["full_signature_correct"] for row in rows))
    return result


def audit(checkpoint, train_release, validation_release, output, *, games_per_matchup=1,
          seed=42, device="cpu"):
    checkpoint, train_release, validation_release, output = map(
        Path, (checkpoint, train_release, validation_release, output))
    if output.exists():
        raise FileExistsError(output)
    if games_per_matchup < 1 or device not in ("cpu", "cuda"):
        raise ValueError("invalid cadence action audit limits")
    if device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    train_sha, validation_sha = validate_release_pair(train_release, validation_release)
    source = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if source.get("source_identity_sha256") != train_sha:
        raise ValueError("teacher does not belong to the training release")
    state = source["state_dict"]
    conditional = "conditional_kind.weight" in state
    model = (ConditionalWholeGameModel if conditional else WholeGameModel)(
        width=state["memory.weight_hh"].shape[1],
        mixture_components=state["position.weight"].shape[0] // 5)
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
    games, cadence, action_windows = Counter(), Counter(), Counter()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                validation_release, "validation", game_ids=chosen):
            matchup = record["matchup"]
            games[matchup] += 1
            terrain = static_grid(load_terrain(directory))
            memory = None
            pending = None
            for observation, target in trajectory_shard(directory):
                if target["update_memory"]:
                    if pending is not None:
                        scored = score_window(pending)
                        if scored is not None:
                            rows.append(scored)
                            action_windows[matchup] += 1
                    encoded, ids, _ = encode_observation(observation, terrain)
                    prediction = model({name: value.to(device)
                                        for name, value in encoded.items()}, memory)
                    memory = prediction["memory"]
                    own = [entity_id for entity_id, index in ids.items()
                           if encoded["relation"][0, index] == 0]
                    if not own:
                        raise ValueError("cadence observation lacks owned actors")
                    actor_id = max(own, key=lambda entity_id:
                                   float(prediction["actor"][0, ids[entity_id]]))
                    kind_logits = prediction["kind"][0].cpu()
                    mode_logits = prediction["target_mode"][0].cpu()
                    joint_kind, joint_mode = select_supported_pair(kind_logits, mode_logits)
                    visible_ids = {entity["id"] for entity in observation["entities"]
                                   if entity["visible"] and entity["id"] in ids}
                    target_id = (max(visible_ids, key=lambda entity_id:
                                     float(prediction["target"][0, ids[entity_id]]))
                                 if visible_ids else None)
                    mixture = prediction["position"][0].cpu()
                    position_components = [
                        (float(torch.sigmoid(component[1])) * terrain.shape[1] * 32,
                         float(torch.sigmoid(component[2])) * terrain.shape[0] * 32)
                        for component in mixture]
                    argument_predictions = dict(
                        target_mode=TARGET_MODES[int(mode_logits.argmax())],
                        target_entity=target_id,
                        position_components=position_components,
                        position_component=int(mixture[:, 0].argmax()),
                        queued=bool(prediction["queued"].argmax(-1).item()),
                        **{name: int(prediction[name].argmax(-1).item()) for name in
                           ("order", "unit_type", "technology", "upgrade", "queue_slot")})
                    pending = dict(game_id=record["game_id"], matchup=matchup,
                                   frame=observation["frame"], event=target["event"],
                                   own_ids=set(own), actor_id=actor_id,
                                   visible_ids=visible_ids,
                                   argument_predictions=argument_predictions,
                                   predicted_kind=KINDS[int(kind_logits.argmax())],
                                   joint_kind=joint_kind, joint_mode=joint_mode,
                                   event_probability=float(torch.sigmoid(
                                       prediction["event"])[0]), labels=[])
                    if target["event"] is not None:
                        cadence[matchup] += 1
                elif target["action"] is not None and pending is not None and \
                        observation["frame"] < pending["frame"] + 24:
                    pending["labels"].append(target["action"])
            if pending is not None:
                scored = score_window(pending)
                if scored is not None:
                    rows.append(scored)
                    action_windows[matchup] += 1
    if any(games[matchup] != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("disjoint validation cohort incomplete")
    report = dict(schema=SCHEMA, checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                  training_identity_sha256=train_sha, validation_identity_sha256=validation_sha,
                  model_family="actor_conditional" if conditional else "baseline",
                  device=device, games=dict(games), strength_validated=False,
                  overall=summarize(rows, sum(cadence.values()), sum(action_windows.values())),
                  by_matchup={matchup: summarize(
                      [row for row in rows if row["matchup"] == matchup],
                      cadence[matchup], action_windows[matchup]) for matchup in MATCHUPS},
                  rows=rows)
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
