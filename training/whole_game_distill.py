"""GPU distillation of a whole-game teacher into the compiled policy width.

This is an offline candidate fit. Export and tournament promotion still require
numerical parity, complete-frame timing, legal command control and game results.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import math
from pathlib import Path
import random

import torch
from torch.nn import functional as F

from .whole_game_batch import collate_observations
from .whole_game_encoding_cache import ObservationCache
from .whole_game_fit import (SCHEMA as FIT_SCHEMA, MATCHUPS, choose_games, collect,
                             sample_loss, summarize_validation, validate_release_pair)
from .whole_game_cadence_collect import collect_cadence_actions
from .whole_game_conditional_model import ConditionalWholeGameModel
from .whole_game_features import encode_label
from .whole_game_model import FORECAST_BINARY_TARGETS, WholeGameModel, masked_action_loss
from .whole_game_quality import load_index
from .whole_game_release import key


SCHEMA = "protodd-whole-game-distill-v1"


def checkpoint_model(path, device):
    checkpoint = torch.load(path, map_location="cpu", weights_only=True)
    if checkpoint.get("schema") != FIT_SCHEMA:
        raise ValueError("unsupported whole-game teacher checkpoint")
    state = checkpoint["state_dict"]
    width = state["memory.weight_hh"].shape[1]
    components = state["position.weight"].shape[0] // 5
    family = ConditionalWholeGameModel if "conditional_kind.weight" in state else WholeGameModel
    model = family(width=width, mixture_components=components)
    model.load_state_dict(state, strict=True)
    return checkpoint, model.to(device).eval()


def cadence_action_buckets(release, quality, args):
    """Choose the same bounded training cohort, then label playable cadence rows."""
    if quality is None:
        raise ValueError("cadence distillation needs a quality index")
    identity = json.loads((release / "identity.json").read_text(encoding="utf-8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "train" and record["matchup"] in MATCHUPS and
                (release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "train", args.games_per_matchup, args.seed,
                             quality, args.high_mmr_threshold, args.high_mmr_share)
    group = {matchup: [record["game_id"] for record in eligible[matchup]
                       if record["game_id"] in chosen] for matchup in MATCHUPS}
    if any(len(group[matchup]) != args.games_per_matchup for matchup in MATCHUPS):
        raise ValueError("cadence distillation cohort incomplete")
    return collect_cadence_actions(
        release, group, quality, history=args.history,
        bucket_limit=args.bucket_limit,
        per_game_bucket_limit=args.per_game_bucket_limit,
        high_mmr_threshold=args.high_mmr_threshold,
        high_mmr_share=args.high_mmr_share, seed=args.seed)


def paired_forward(teacher, student, samples, device, cache=None):
    """Use the same causal observations and right-aligned history for both nets."""
    terrains = [sample[3] for sample in samples]
    max_history = max(len(sample[0]) for sample in samples)
    teacher_memory = student_memory = None
    for step in range(max_history):
        valid = [step >= max_history - len(sample[0]) for sample in samples]
        rows = [sample[0][step - max_history + len(sample[0])] if active else sample[1]
                for sample, active in zip(samples, valid)]
        batch, _, _ = collate_observations(rows, terrains, device, cache)
        with torch.no_grad():
            proposed_teacher = teacher(batch, teacher_memory)["memory"]
        proposed_student = student(batch, student_memory)["memory"]
        mask = torch.tensor(valid, dtype=torch.bool, device=device)[:, None]
        teacher_memory = torch.where(mask, proposed_teacher,
                                     torch.zeros_like(proposed_teacher) if teacher_memory is None else teacher_memory)
        student_memory = torch.where(mask, proposed_student,
                                     torch.zeros_like(proposed_student) if student_memory is None else student_memory)
    batch, ids, _ = collate_observations([sample[1] for sample in samples], terrains, device, cache)
    with torch.no_grad():
        teacher_output = teacher(batch, teacher_memory)
    return teacher_output, student(batch, student_memory), batch, ids


def categorical_kl(student, teacher, temperature=2.0):
    return F.kl_div(F.log_softmax(student.float() / temperature, dim=-1),
                    F.softmax(teacher.float() / temperature, dim=-1),
                    reduction="batchmean") * temperature ** 2


def distillation_loss(teacher_output, student_output, batch, ids, samples):
    """Imitate teacher decisions only where that head has observed supervision."""
    target = samples[0][2]
    task = "forecast" if "forecast" in target else "event" if target["update_memory"] else "action"
    if task == "event":
        return F.binary_cross_entropy_with_logits(student_output["event"].float(),
                                                  torch.sigmoid(teacher_output["event"].float()))
    if task == "forecast":
        horizon = str(target["horizon"])
        student = student_output[f"forecast_{horizon}"].float()
        teacher = teacher_output[f"forecast_{horizon}"].float()
        return (F.binary_cross_entropy_with_logits(student[:, :-1], torch.sigmoid(teacher[:, :-1]))
                + F.smooth_l1_loss(student[:, -1], teacher[:, -1]))
    masks = [sample[2]["action"]["loss_masks"] for sample in samples]
    pieces = [categorical_kl(student_output["domain"], teacher_output["domain"]),
              categorical_kl(student_output["kind"], teacher_output["kind"])]
    for name in ("queued", "target_mode", "order", "unit_type", "technology", "upgrade", "queue_slot"):
        selected = [index for index, mask in enumerate(masks) if mask.get(name, False)]
        if selected:
            pieces.append(categorical_kl(student_output[name][selected], teacher_output[name][selected]))
    actor_known = batch["entity_mask"] & (batch["relation"] == 0)
    for index, sample in enumerate(samples):
        unknown = set(sample[2]["action"]["actor_unknown"])
        for position, entity_id in enumerate(ids[index]):
            if entity_id in unknown:
                actor_known[index, position] = False
    if actor_known.any():
        pieces.append(F.binary_cross_entropy_with_logits(
            student_output["actor"][actor_known].float(),
            torch.sigmoid(teacher_output["actor"][actor_known].float())))
    target_rows = [index for index, mask in enumerate(masks) if mask.get("target_entity", False)]
    for index in target_rows:
        legal = batch["entity_mask"][index]
        pieces.append(categorical_kl(student_output["target"][index, legal][None],
                                     teacher_output["target"][index, legal][None]))
    position_rows = [index for index, mask in enumerate(masks) if mask.get("target_position", False)]
    if position_rows:
        # Both models use the same ordered mixture-head slots; matching raw
        # parameters preserves multimodality while supervised density loss
        # still decides where the student should place probability mass.
        pieces.append(F.smooth_l1_loss(student_output["position"][position_rows].float(),
                                       teacher_output["position"][position_rows].float()))
    return torch.stack(pieces).mean()


def supervised_output_loss(output, samples, ids, device):
    """Exact minibatch supervision on the already computed student forward."""
    target = samples[0][2]
    task = "forecast" if "forecast" in target else "event" if target["update_memory"] else "action"
    if task == "event":
        truth = torch.tensor([sample[2]["event"] for sample in samples],
                             dtype=torch.float32, device=device)
        return F.binary_cross_entropy_with_logits(output["event"], truth)
    if task == "forecast":
        horizons = {sample[2]["horizon"] for sample in samples}
        if len(horizons) != 1:
            raise ValueError("forecast minibatch needs one horizon")
        prediction = output[f"forecast_{next(iter(horizons))}"]
        truths, magnitudes = [], []
        for sample in samples:
            future = sample[2]["forecast"]
            truth = [int(future["newly_explored_tiles"] > 0),
                     int(future["newly_known_enemy_ids_retained"] > 0),
                     int(future["own_hp_shield_loss_common"] > 0),
                     int(future["technology_completed_gain"] + future["upgrade_level_gain"] > 0),
                     int(future["visible_enemies_at_target"] > 0),
                     *future["confirmed_command_domains"]]
            if len(truth) != len(FORECAST_BINARY_TARGETS):
                raise ValueError("forecast target width changed")
            truths.append(truth)
            magnitudes.append(math.log1p(future["newly_explored_tiles"]) /
                              math.log1p(len(sample[1]["vision"])))
        binary = torch.tensor(truths, dtype=torch.float32, device=device)
        magnitude = torch.tensor(magnitudes, dtype=torch.float32, device=device)
        return (F.binary_cross_entropy_with_logits(prediction[:, :-1], binary) +
                F.smooth_l1_loss(prediction[:, -1], magnitude))
    losses = []
    for index, sample in enumerate(samples):
        terrain = sample[3]
        label = encode_label(sample[2]["action"], ids[index],
                             terrain.shape[1], terrain.shape[0])
        for name in ("actor_known", "actor_positive"):
            label[name] = F.pad(label[name],
                                (0, output["actor"].shape[1] - label[name].shape[1]))
        label = {name: value.to(device) if isinstance(value, torch.Tensor) else value
                 for name, value in label.items()}
        single = {name: value[index:index + 1] for name, value in output.items()}
        loss, _ = masked_action_loss(single, label)
        losses.append(loss)
    return torch.stack(losses).mean()


def fit(args):
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required for distillation")
    if args.output.exists():
        raise FileExistsError(args.output)
    source_hashes = {name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                     for name in ("whole_game_distill.py", "whole_game_batch.py",
                                  "whole_game_model.py", "whole_game_features.py",
                                  "whole_game_encoding_cache.py",
                                  "whole_game_conditional_model.py",
                                  "whole_game_cadence_collect.py", "whole_game_fit.py",
                                  "whole_game_quality.py", "whole_game_release.py",
                                  "whole_game_shards.py", "whole_game_sequences.py",
                                  "whole_game_future.py")}
    teacher_sha = hashlib.sha256(args.teacher.read_bytes()).hexdigest()
    train_sha, validation_sha = validate_release_pair(args.release, args.validation_release)
    device = torch.device("cuda")
    checkpoint, teacher = checkpoint_model(args.teacher, device)
    if checkpoint["source_identity_sha256"] != train_sha:
        raise ValueError("teacher checkpoint does not belong to this training release")
    if args.width >= teacher.width:
        raise ValueError("student must be narrower than teacher")
    quality = load_index(args.quality_index, args.release / "identity.json") if args.quality_index else None
    validation_quality = (load_index(args.validation_quality_index,
                          args.validation_release / "identity.json") if args.validation_quality_index else None)
    print(json.dumps(dict(stage="collect", teacher_width=teacher.width,
                          student_width=args.width, games_per_matchup=args.games_per_matchup)), flush=True)
    train, train_info = collect(args.release, "train", games_per_matchup=args.games_per_matchup,
                                history=args.history, bucket_limit=args.bucket_limit, seed=args.seed,
                                quality_index=quality, high_mmr_threshold=args.high_mmr_threshold,
                                high_mmr_share=args.high_mmr_share,
                                per_game_bucket_limit=args.per_game_bucket_limit)
    cadence_info = None
    if args.action_source == "cadence":
        cadence, cadence_info = cadence_action_buckets(args.release, quality, args)
        cadence_info = dict(cadence_info,
                            first_kind_counts={"/".join(category): count for category, count
                                               in cadence_info["first_kind_counts"].items()},
                            event_counts={matchup: dict(counts) for matchup, counts
                                          in cadence_info["event_counts"].items()})
        train = {category: samples for category, samples in train.items()
                 if category[1] in ("event", "forecast")}
        if set(train) & set(cadence):
            raise ValueError("cadence actions collide with timing samples")
        train.update(cadence)
    validation, validation_info = collect(args.validation_release, "validation",
                                          games_per_matchup=args.games_per_matchup,
                                          history=args.history, bucket_limit=args.bucket_limit, seed=args.seed,
                                          quality_index=validation_quality,
                                          per_game_bucket_limit=args.per_game_bucket_limit)
    keys = sorted(train)
    task_keys = dict(event=[k for k in keys if k[1] == "event"],
                     action=[k for k in keys if k[1] not in ("event", "forecast")],
                     forecast=[k for k in keys if k[1] == "forecast"])
    if any(not choices for choices in task_keys.values()):
        raise ValueError("distillation requires event, action and forecast samples")
    args.output.mkdir(parents=True)
    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)
    rng = random.Random(args.seed)
    student = WholeGameModel(width=args.width,
                             mixture_components=teacher.mixture_components).to(device).train()
    optimizer = torch.optim.AdamW(student.parameters(), lr=args.learning_rate)
    steps_by_task = Counter()
    examples_by_task = Counter()
    losses = defaultdict(list)
    encoding_cache = ObservationCache(args.encoding_cache_mb * 1024 * 1024)
    tasks = list(task_keys)
    print(json.dumps(dict(stage="fit", train_games=train_info["games"],
                          validation_games=validation_info["games"], steps=args.steps)), flush=True)
    for step in range(args.steps):
        task = tasks[step % len(tasks)]
        category = task_keys[task][steps_by_task[task] % len(task_keys[task])]
        steps_by_task[task] += 1
        anchor = rng.choice(train[category])
        compatible = [sample for sample in train[category] if sample[3].shape == anchor[3].shape]
        samples = rng.sample(compatible, min(args.batch_size, len(compatible)))
        optimizer.zero_grad(set_to_none=True)
        with torch.autocast(device_type="cuda", dtype=torch.bfloat16, enabled=not args.no_amp):
            teacher_output, student_output, batch, ids = paired_forward(
                teacher, student, samples, device, encoding_cache)
            imitation = supervised_output_loss(student_output, samples, ids, device)
            distillation = distillation_loss(teacher_output, student_output, batch, ids, samples)
            loss = imitation + args.distill_weight * distillation
        if not torch.isfinite(loss):
            raise ValueError(f"nonfinite distillation loss at step {step}")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(student.parameters(), 1.0)
        optimizer.step()
        examples_by_task[task] += len(samples)
        losses[task].append(float(loss.detach().cpu()))
        if (step + 1) % 64 == 0:
            print(json.dumps(dict(stage="fit", step=step + 1,
                                  mean_recent_loss=sum(losses[task][-32:]) /
                                  len(losses[task][-32:]))), flush=True)
    student.eval()
    validation_rows = []
    with torch.no_grad():
        for category in sorted(validation):
            for sample in validation[category]:
                loss, metrics = sample_loss(student, sample, device)
                validation_rows.append(dict(category="/".join(category),
                                            loss=float(loss.detach().cpu()),
                                            game_id=sample[4]["game_id"],
                                            quality_band=sample[4]["quality_band"], **metrics))
    if not validation_rows:
        raise ValueError("empty distillation validation set")
    report = dict(schema=SCHEMA, experimental=True, training_ready=False,
                  strength_validated=False, deployment="none",
                  teacher_checkpoint_sha256=teacher_sha,
                  teacher_family="actor_conditional" if isinstance(
                      teacher, ConditionalWholeGameModel) else "baseline",
                  teacher_width=teacher.width, student_width=args.width,
                  source_code_sha256=source_hashes,
                  source_identity_sha256=train_sha,
                  validation_identity_sha256=validation_sha,
                  training=train_info, validation=validation_info,
                  training_action_observation=args.action_source,
                  validation_action_observation="before_command",
                  cadence_training=cadence_info,
                  steps=args.steps, batch_size=args.batch_size,
                  history=args.history, distill_weight=args.distill_weight,
                  training_steps_by_task=dict(steps_by_task),
                  training_examples_by_task=dict(examples_by_task),
                  encoding_cache=encoding_cache.stats(),
                  mean_train_loss_by_task={task: sum(values) / len(values)
                                           for task, values in losses.items()},
                  mean_validation_loss=sum(row["loss"] for row in validation_rows) /
                  len(validation_rows),
                  balanced_validation=summarize_validation(validation_rows),
                  validation_by_quality={band: summarize_validation([
                      row for row in validation_rows if row["quality_band"] == band])
                      for band in ("base", "high")} if validation_quality else None,
                  validation_rows=validation_rows,
                  gpu=torch.cuda.get_device_name(),
                  gpu_peak_bytes=torch.cuda.max_memory_allocated())
    # The existing deterministic weight exporter accepts the same model-state
    # schema as a direct fit. The separate report records this as distillation.
    torch.save(dict(schema=FIT_SCHEMA, state_dict=student.cpu().state_dict(),
                    source_identity_sha256=train_sha,
                    distillation_schema=SCHEMA), args.output / "student.pt")
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--teacher", required=True, type=Path)
    parser.add_argument("--release", required=True, type=Path)
    parser.add_argument("--validation-release", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--quality-index", type=Path)
    parser.add_argument("--validation-quality-index", type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=64)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--bucket-limit", type=int, default=64)
    parser.add_argument("--per-game-bucket-limit", type=int, default=2)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--encoding-cache-mb", type=int, default=512)
    parser.add_argument("--steps", type=int, default=1024)
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--distill-weight", type=float, default=0.5)
    parser.add_argument("--high-mmr-threshold", type=int, default=2300)
    parser.add_argument("--high-mmr-share", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-amp", action="store_true")
    parser.add_argument("--action-source", choices=("before_command", "cadence"),
                        default="before_command")
    args = parser.parse_args()
    if (args.games_per_matchup < 1 or args.history < 1 or args.bucket_limit < 1 or
            args.per_game_bucket_limit < 1 or args.batch_size < 1 or args.steps < 1 or
            args.encoding_cache_mb < 1 or
            args.width < 64 or args.learning_rate <= 0 or args.distill_weight < 0 or
            not 0 <= args.high_mmr_share <= 1):
        parser.error("invalid distillation configuration")
    report = fit(args)
    print(json.dumps(dict(output=str(args.output.resolve()),
                          mean_validation_loss=report["mean_validation_loss"],
                          gpu_peak_bytes=report["gpu_peak_bytes"])), flush=True)


if __name__ == "__main__":
    main()
