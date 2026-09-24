"""Inspect whole-game action confusions on a disjoint replay validation release."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
import math
from pathlib import Path
import random

import torch

from .whole_game_action_schema import KIND_TARGET_MODES, select_supported_pair
from .whole_game_conditional_model import ConditionalWholeGameModel
from .whole_game_features import encode_label, encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, collect, validate_release_pair
from .whole_game_model import DOMAINS, KINDS, TARGET_MODES, WholeGameModel
from .whole_game_quality import load_index
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


SCHEMA = "protodd-whole-game-action-audit-v1"


def training_kind_prior(release, selection_from):
    """Read kind frequencies only from hash-verified frozen training shards."""
    run = json.loads(Path(selection_from).read_text(encoding="utf8"))
    identity_sha = hashlib.sha256((Path(release) / "identity.json").read_bytes()).hexdigest()
    if run["source_identity_sha256"] != identity_sha:
        raise ValueError("training cohort belongs to a different release")
    chosen = {game_id for group in run["groups"] for ids in group.values() for game_id in ids}
    counts = Counter()
    found = set()
    for record, _, receipt in selected_shards(release, "train", game_ids=chosen):
        found.add(record["game_id"])
        counts.update(receipt["report"]["labels"]["kinds"])
    if found != chosen or not counts:
        raise ValueError("frozen training cohort is missing verified receipts")
    total = sum(counts.values()) + len(KINDS)
    bias = torch.tensor([math.log((counts[kind] + 1) / total) for kind in KINDS])
    return bias, dict(games=len(found), candidates=sum(counts.values()), kinds=dict(counts))


def collect_natural_actions(release, *, games_per_matchup, actions_per_matchup,
                            history, quality_index, seed):
    """Uniformly reservoir-sample actual commands, without kind balancing."""
    if games_per_matchup < 1 or actions_per_matchup < 1 or history < 1:
        raise ValueError("natural audit limits must be positive")
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "validation" and record["matchup"] in MATCHUPS and
                (release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "validation", games_per_matchup, seed)
    rng = {matchup: random.Random(f"{seed}:{matchup}:natural") for matchup in MATCHUPS}
    buckets = {(matchup, "action"): [] for matchup in MATCHUPS}
    games, seen = Counter(), Counter()
    for record, directory, _ in selected_shards(release, "validation", game_ids=chosen):
        matchup = record["matchup"]
        games[matchup] += 1
        terrain = static_grid(load_terrain(directory))
        recent = deque(maxlen=history)
        quality = quality_index["games"][record["game_id"]] if quality_index else None
        meta = dict(game_id=record["game_id"],
                    mmr_claim=quality["mmr_claim"] if quality else None,
                    quality_band=("high" if quality["mmr_claim"] >= 2300 else "base")
                    if quality else None)
        for row, target in trajectory_shard(directory):
            if target["update_memory"]:
                recent.append(row)
            elif target["action"] is not None:
                seen[matchup] += 1
                bucket = buckets[(matchup, "action")]
                sample = (list(recent), row, target, terrain, meta)
                if len(bucket) < actions_per_matchup:
                    bucket.append(sample)
                else:
                    pick = rng[matchup].randrange(seen[matchup])
                    if pick < actions_per_matchup:
                        bucket[pick] = sample
    if any(games[m] != games_per_matchup or not buckets[(m, "action")] for m in MATCHUPS):
        raise ValueError("natural audit needs complete held-out games in all matchups")
    return buckets, dict(games=dict(games), observed_actions=dict(seen),
                         sampled_actions={m: len(buckets[(m, "action")]) for m in MATCHUPS})


def summarize(records):
    if not records:
        raise ValueError("no held-out action examples")
    true_kinds = Counter(row["true_kind"] for row in records)
    predicted_kinds = Counter(row["predicted_kind"] for row in records)
    by_kind = {}
    for name in sorted(true_kinds):
        subset = [row for row in records if row["true_kind"] == name]
        by_kind[name] = dict(samples=len(subset),
                             top1_correct=sum(row["predicted_kind"] == name for row in subset),
                             predicted_as=dict(Counter(row["predicted_kind"] for row in subset)))
    actionable = [row for row in records if row["true_kind"] in KIND_TARGET_MODES]
    mode_known = [row for row in records if row.get("target_mode_known")]
    entity_known = [row for row in records if row.get("target_entity_known")]
    position_known = [row for row in records if row.get("target_position_known")]
    unit_type_known = [row for row in records if row.get("unit_type_known")]
    return dict(samples=len(records),
                kind_top1_correct=sum(row["predicted_kind"] == row["true_kind"] for row in records),
                kind_top5_correct=sum(row.get("kind_rank", 1000) <= 5 for row in records),
                actionable_samples=len(actionable),
                actionable_kind_top1_correct=sum(row["predicted_kind"] == row["true_kind"]
                                                 for row in actionable),
                actionable_kind_top5_correct=sum(row.get("kind_rank", 1000) <= 5
                                                 for row in actionable),
                target_mode_known=len(mode_known),
                target_mode_top1_correct=sum(row["predicted_target_mode"] == row["true_target_mode"]
                                             for row in mode_known),
                kind_target_shape_compatible=sum(row.get("kind_target_shape_compatible", False)
                                                 for row in records),
                supported_joint_kind_top1_correct=sum(row.get("supported_joint_kind") == row["true_kind"]
                                                      for row in records),
                supported_joint_pair_top1_correct=sum(
                    row.get("supported_joint_kind") == row["true_kind"] and
                    row.get("supported_joint_target_mode") == row["true_target_mode"]
                    for row in mode_known),
                domain_top1_correct=sum(row["predicted_domain"] == row["true_domain"] for row in records),
                actor_top1_known=sum(row["actor_known"] for row in records),
                actor_top1_correct=sum(row["actor_correct"] for row in records),
                target_entity_known=len(entity_known),
                target_entity_top1_correct=sum(row["target_entity_correct"] for row in entity_known),
                target_position_known=len(position_known),
                target_position_within_64px=sum(row["target_position_error_px"] <= 64
                                                for row in position_known),
                target_position_oracle_within_64px=sum(row["target_position_oracle_error_px"] <= 64
                                                       for row in position_known),
                unit_type_known=len(unit_type_known),
                unit_type_top1_correct=sum(row["unit_type_correct"] for row in unit_type_known),
                full_signature_correct=sum(row.get("full_signature_correct", False) for row in records),
                true_kinds=dict(true_kinds), predicted_kinds=dict(predicted_kinds),
                by_true_kind=by_kind)


def audit(checkpoint_path, train_release, release, output, *, games_per_matchup=1, bucket_limit=64,
          per_game_bucket_limit=2, history=8, quality_index=None, seed=42,
          sample_mode="balanced", actions_per_matchup=96,
          training_selection=None, kind_bias_strength=0.0):
    checkpoint_path, train_release, release, output = map(Path, (checkpoint_path, train_release, release, output))
    if output.exists():
        raise FileExistsError(output)
    checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=True)
    train_identity_sha, validation_identity_sha = validate_release_pair(train_release, release)
    if checkpoint.get("source_identity_sha256") != train_identity_sha:
        raise ValueError("checkpoint does not belong to the disjoint training release")
    state = checkpoint["state_dict"]
    width = state["memory.weight_hh"].shape[1]
    components = state["position.weight"].shape[0] // 5
    conditional = "conditional_kind.weight" in state
    model = (ConditionalWholeGameModel if conditional else WholeGameModel)(
        width=width, mixture_components=components)
    model.load_state_dict(state, strict=True)
    model.eval()
    if not math.isfinite(kind_bias_strength):
        raise ValueError("kind bias strength must be finite")
    if kind_bias_strength and training_selection is None:
        raise ValueError("kind bias requires a frozen training selection")
    prior = None
    kind_bias = None
    if training_selection is not None:
        kind_bias, prior = training_kind_prior(train_release, training_selection)
    quality = load_index(quality_index, release / "identity.json") if quality_index else None
    if sample_mode == "balanced":
        buckets, selection = collect(release, "validation", games_per_matchup=games_per_matchup,
                                     history=history, bucket_limit=bucket_limit, seed=seed,
                                     forecast=False, quality_index=quality,
                                     per_game_bucket_limit=per_game_bucket_limit)
    elif sample_mode == "natural":
        buckets, selection = collect_natural_actions(
            release, games_per_matchup=games_per_matchup,
            actions_per_matchup=actions_per_matchup, history=history,
            quality_index=quality, seed=seed)
    else:
        raise ValueError("unknown audit sample mode")
    records = []
    with torch.no_grad():
        for category, samples in sorted(buckets.items()):
            if category[1] == "event":
                continue
            for sample in samples:
                context, row, supervision, terrain = sample[:4]
                label = supervision["action"]
                memory = None
                for previous in context:
                    batch, _, _ = encode_observation(previous, terrain)
                    memory = model(batch, memory)["memory"]
                batch, ids, _ = encode_observation(row, terrain)
                prediction = model(batch, memory)
                inverse = {position: entity_id for entity_id, position in ids.items()}
                own = [position for position in ids.values()
                       if batch["relation"][0, position].item() == 0]
                actor_top = max(own, key=lambda position: prediction["actor"][0, position].item())
                actor_known = bool(label["actor_positive"])
                kind_logits = prediction["kind"][0]
                if kind_bias is not None:
                    kind_logits = kind_logits + kind_bias_strength * kind_bias
                true_kind = label["actions"]["kind"]
                true_kind_index = KINDS.index(true_kind)
                predicted_kind = KINDS[kind_logits.argmax().item()]
                mode_logits = prediction["target_mode"][0]
                predicted_target_mode = TARGET_MODES[mode_logits.argmax().item()]
                joint_kind, joint_mode = select_supported_pair(kind_logits, mode_logits)
                target_mode_known = bool(label["loss_masks"]["target_mode"])
                encoded_label = encode_label(label, ids, terrain.shape[1], terrain.shape[0])
                target_entity_known = bool(encoded_label["mask"].get("target_entity", False))
                target_entity_correct = False
                if target_entity_known:
                    visible = {entity["id"] for entity in row["entities"] if entity["visible"]}
                    visible_positions = [position for entity_id, position in ids.items()
                                         if entity_id in visible]
                    if visible_positions:
                        predicted_target = max(visible_positions,
                                               key=lambda position: prediction["target"][0, position].item())
                        target_entity_correct = predicted_target == encoded_label["target_entity"].item()
                target_position_known = bool(encoded_label["mask"].get("target_position", False))
                target_position_error_px = None
                target_position_oracle_error_px = None
                if target_position_known:
                    mixture = prediction["position"][0]
                    component = mixture[:, 0].argmax().item()
                    errors = torch.linalg.vector_norm(
                        (mixture[:, 1:3].sigmoid() - encoded_label["target_position"][0]) *
                        torch.tensor([terrain.shape[1] * 32, terrain.shape[0] * 32]), dim=-1)
                    target_position_error_px = float(errors[component])
                    target_position_oracle_error_px = float(errors.min())
                unit_type_known = bool(encoded_label["mask"].get("unit_type", False))
                unit_type_correct = unit_type_known and prediction["unit_type"].argmax(-1).item() == \
                    encoded_label["unit_type"].item()
                full_signature_correct = (predicted_kind == true_kind and
                                          (not target_mode_known or predicted_target_mode ==
                                           label["actions"]["target_mode"]) and
                                          (not actor_known or inverse[actor_top] in label["actor_positive"]) and
                                          (not target_entity_known or target_entity_correct) and
                                          (not target_position_known or target_position_error_px <= 64) and
                                          (not unit_type_known or unit_type_correct))
                records.append(dict(game_id=sample[4]["game_id"], frame=row["frame"],
                                    matchup=category[0], quality_band=sample[4]["quality_band"],
                                    true_domain=label["domain"],
                                    predicted_domain=DOMAINS[prediction["domain"].argmax(-1).item()],
                                    true_kind=true_kind, predicted_kind=predicted_kind,
                                    kind_rank=1 + int((kind_logits > kind_logits[true_kind_index]).sum()),
                                    predicted_kind_probability=float(kind_logits.softmax(-1).max()),
                                    target_mode_known=target_mode_known,
                                    true_target_mode=label["actions"]["target_mode"]
                                    if target_mode_known else None,
                                    predicted_target_mode=predicted_target_mode,
                                    supported_joint_kind=joint_kind,
                                    supported_joint_target_mode=joint_mode,
                                    kind_target_shape_compatible=predicted_target_mode in
                                    KIND_TARGET_MODES.get(predicted_kind, ()),
                                    actor_known=actor_known,
                                    actor_correct=actor_known and
                                    inverse[actor_top] in label["actor_positive"],
                                    target_entity_known=target_entity_known,
                                    target_entity_correct=target_entity_correct,
                                    target_position_known=target_position_known,
                                    target_position_error_px=target_position_error_px,
                                    target_position_oracle_error_px=target_position_oracle_error_px,
                                    unit_type_known=unit_type_known,
                                    unit_type_correct=unit_type_correct,
                                    full_signature_correct=full_signature_correct))
    report = dict(schema=SCHEMA, sample_mode=sample_mode,
                  model_family="actor_conditional" if conditional else "baseline",
                  kind_bias_strength=kind_bias_strength, training_kind_prior=prior,
                  checkpoint_sha256=hashlib.sha256(checkpoint_path.read_bytes()).hexdigest(),
                  validation_identity_sha256=validation_identity_sha,
                  games=selection["games"], selection=selection, overall=summarize(records),
                  by_matchup={matchup: summarize([row for row in records if row["matchup"] == matchup])
                              for matchup in ("PvT", "PvZ", "PvP") if any(row["matchup"] == matchup for row in records)},
                  rows=records, strength_validated=False)
    output.mkdir(parents=True)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("train_release", type=Path)
    parser.add_argument("release", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--quality-index", type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=1)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--bucket-limit", type=int, default=64)
    parser.add_argument("--per-game-bucket-limit", type=int, default=2)
    parser.add_argument("--sample-mode", choices=("balanced", "natural"), default="balanced")
    parser.add_argument("--actions-per-matchup", type=int, default=96)
    parser.add_argument("--training-selection", type=Path)
    parser.add_argument("--kind-bias-strength", type=float, default=0.0)
    args = parser.parse_args()
    result = audit(args.checkpoint, args.train_release, args.release, args.output,
                   games_per_matchup=args.games_per_matchup, history=args.history,
                   bucket_limit=args.bucket_limit,
                   per_game_bucket_limit=args.per_game_bucket_limit,
                   quality_index=args.quality_index, sample_mode=args.sample_mode,
                   actions_per_matchup=args.actions_per_matchup,
                   training_selection=args.training_selection,
                   kind_bias_strength=args.kind_bias_strength)
    print(json.dumps(dict(output=str(args.output.resolve()), overall=result["overall"]), indent=2))


if __name__ == "__main__":
    main()
