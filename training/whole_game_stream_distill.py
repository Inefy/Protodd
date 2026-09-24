"""Resumable GPU distillation from a whole-game teacher to the CPU policy width.

Train actions are observed at deployable cadence. Events use natural train
frequency; action updates blend natural and rare kinds. One replay group is
held in memory at a time. No student is deployable without independent cadence,
CPU parity, callback-latency and paired-game gates.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import gc
import hashlib
import json
import os
from pathlib import Path
import random
import time

os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
import torch

from .whole_game_action_sampling import group_seed, mixed_action_schedule
from .whole_game_cadence_collect import collect_cadence_actions
from .whole_game_distill import (checkpoint_model, distillation_loss, paired_forward,
                                 supervised_output_loss)
from .whole_game_encoding_cache import ObservationCache
from .whole_game_event_sampling import natural_event_schedule
from .whole_game_fit import MATCHUPS, collect, sample_loss, summarize_validation, validate_release_pair
from .whole_game_model import WholeGameModel
from .whole_game_quality import load_index
from .whole_game_stream_fit import _collect_group, plan_chunks, verify_chunks


SCHEMA = "protodd-whole-game-stream-distill-v1"
SOURCE_FILES = ("whole_game_stream_distill.py", "whole_game_distill.py",
                "whole_game_stream_fit.py", "whole_game_fit.py", "whole_game_batch.py",
                "whole_game_model.py", "whole_game_conditional_model.py",
                "whole_game_features.py", "whole_game_encoding_cache.py",
                "whole_game_cadence_collect.py", "whole_game_action_sampling.py",
                "whole_game_event_sampling.py", "whole_game_quality.py",
                "whole_game_release.py", "whole_game_shards.py",
                "whole_game_sequences.py", "whole_game_future.py")
TASKS = ("action", "action", "event", "forecast")


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _save(path, value):
    temporary = path.with_suffix(".tmp")
    torch.save(value, temporary)
    os.replace(temporary, path)


def group_buckets(args, group, quality, seed):
    buckets, info = _collect_group(args, group, quality, seed)
    buckets = {category: samples for category, samples in buckets.items()
               if category[1] in ("event", "forecast")}
    cadence, cadence_info = collect_cadence_actions(
        args.release, group, quality, history=args.history,
        bucket_limit=args.bucket_limit,
        per_game_bucket_limit=args.per_game_bucket_limit,
        high_mmr_threshold=args.high_mmr_threshold,
        high_mmr_share=args.high_mmr_share, seed=seed)
    if set(buckets) & set(cadence):
        raise ValueError("cadence actions overlap timing or forecast buckets")
    buckets.update(cadence)
    info["cadence_action_windows"] = cadence_info["action_windows"]
    info["cadence_skipped_unavailable_actor"] = cadence_info["skipped_unavailable_actor"]
    return buckets, info, cadence_info


def category_schedule(buckets, cadence_info, group, steps):
    if steps < len(TASKS) or steps % len(TASKS):
        raise ValueError("steps per group must cover complete task cycles")
    categories = sorted(buckets)
    actions = [key for key in categories if key[1] not in ("event", "forecast")]
    events = [key for key in categories if key[1] == "event"]
    forecasts = [key for key in categories if key[1] == "forecast"]
    if not actions or not events or not forecasts:
        raise ValueError("group lacks action, event or forecast samples")
    seed = group_seed(group)
    return dict(
        action=mixed_action_schedule(
            actions, cadence_info["first_kind_counts"], steps // 2,
            natural_share=0.5, seed=seed),
        event=natural_event_schedule(
            events, cadence_info["event_counts"], steps // 4, seed=seed),
        forecast=forecasts)


def fit(args):
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required for streaming distillation")
    if (args.games_per_matchup < 1 or args.chunk_games_per_matchup < 1 or
            args.steps_per_chunk < 4 or args.steps_per_chunk % 4 or
            args.epochs < 1 or args.width < 64 or args.batch_size < 1 or
            args.collect_workers not in (1, 2, 3) or args.encoding_cache_mb < 1 or
            args.learning_rate <= 0 or args.distill_weight < 0 or
            args.validation_games_per_matchup < 1):
        raise ValueError("invalid stream distillation limits")
    torch.use_deterministic_algorithms(True)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cuda.matmul.allow_tf32 = False
    train_sha, validation_sha = validate_release_pair(args.release, args.validation_release)
    quality = load_index(args.quality_index, args.release / "identity.json")
    validation_quality = load_index(args.validation_quality_index,
                                    args.validation_release / "identity.json")
    teacher_checkpoint, teacher = checkpoint_model(args.teacher, torch.device("cuda"))
    if teacher_checkpoint["source_identity_sha256"] != train_sha:
        raise ValueError("teacher belongs to another replay release")
    if args.width >= teacher.width:
        raise ValueError("student must be narrower than teacher")
    sources = {name: _sha(Path(__file__).parent / name) for name in SOURCE_FILES}
    if args.resume:
        previous = json.loads((args.output / "run.json").read_text(encoding="utf-8"))
        groups = previous["groups"]
        quality_counts = previous["quality_selection"]
        verify_chunks(args.release, groups, games_per_matchup=args.games_per_matchup,
                      chunk_games_per_matchup=args.chunk_games_per_matchup)
    else:
        groups, quality_counts = plan_chunks(
            args.release, quality, games_per_matchup=args.games_per_matchup,
            chunk_games_per_matchup=args.chunk_games_per_matchup, seed=args.seed,
            high_mmr_threshold=args.high_mmr_threshold,
            high_mmr_share=args.high_mmr_share)
    spec = dict(schema=SCHEMA, teacher_sha256=_sha(args.teacher),
                teacher_family="actor_conditional" if hasattr(teacher, "conditional_kind")
                else "baseline", source_identity_sha256=train_sha,
                validation_identity_sha256=validation_sha,
                quality_index_sha256=_sha(args.quality_index),
                validation_quality_index_sha256=_sha(args.validation_quality_index),
                source_code_sha256=sources, groups=groups,
                quality_selection=quality_counts,
                games_per_matchup=args.games_per_matchup,
                chunk_games_per_matchup=args.chunk_games_per_matchup,
                validation_games_per_matchup=args.validation_games_per_matchup,
                epochs=args.epochs, steps_per_chunk=args.steps_per_chunk,
                batch_size=args.batch_size, width=args.width, history=args.history,
                bucket_limit=args.bucket_limit,
                per_game_bucket_limit=args.per_game_bucket_limit,
                learning_rate=args.learning_rate, distill_weight=args.distill_weight,
                task_schedule=list(TASKS), seed=args.seed,
                encoding_cache_mb=args.encoding_cache_mb,
                collect_workers=args.collect_workers,
                high_mmr_threshold=args.high_mmr_threshold,
                high_mmr_share=args.high_mmr_share,
                bf16_amp=not args.no_amp)
    spec_digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    if args.resume:
        if previous != spec:
            raise ValueError("resume source, teacher or cohort changed")
    else:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.mkdir(parents=True)
        (args.output / "run.json").write_text(json.dumps(spec, indent=2) + "\n",
                                               encoding="utf-8")
    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)
    student = WholeGameModel(width=args.width,
                             mixture_components=teacher.mixture_components).cuda().train()
    optimizer = torch.optim.AdamW(student.parameters(), lr=args.learning_rate)
    rng = random.Random(args.seed)
    task_steps, task_examples = Counter(), Counter()
    next_group = 0
    if args.resume:
        saved = torch.load(args.output / "resume.pt", map_location="cpu", weights_only=True)
        if saved["spec_digest"] != spec_digest:
            raise ValueError("resume checkpoint differs from run specification")
        student.load_state_dict(saved["model"])
        optimizer.load_state_dict(saved["optimizer"])
        rng.setstate(saved["python_rng"])
        torch.set_rng_state(saved["torch_rng"].cpu())
        torch.cuda.set_rng_state_all(saved["cuda_rng"])
        task_steps.update(saved["task_steps"])
        task_examples.update(saved["task_examples"])
        next_group = saved["next_group"]
    total_groups = args.epochs * len(groups)
    started = time.perf_counter()
    groups_this_run = 0
    for global_group in range(next_group, total_groups):
        epoch, slot = divmod(global_group, len(groups))
        order = list(range(len(groups)))
        random.Random(args.seed + epoch).shuffle(order)
        group = groups[order[slot]]
        buckets, info, cadence_info = group_buckets(args, group, quality, args.seed + epoch)
        if any(info["games"].get(matchup) != len(group[matchup]) for matchup in MATCHUPS):
            raise ValueError("stream group changed during student fit")
        schedule = category_schedule(buckets, cadence_info, group, args.steps_per_chunk)
        cache = ObservationCache(args.encoding_cache_mb * 1024 * 1024)
        losses = defaultdict(list)
        for offset in range(args.steps_per_chunk):
            task = TASKS[offset % len(TASKS)]
            category = schedule[task][task_steps[task] % len(schedule[task])]
            task_steps[task] += 1
            anchor = rng.choice(buckets[category])
            compatible = [sample for sample in buckets[category]
                          if sample[3].shape == anchor[3].shape]
            samples = rng.sample(compatible, min(args.batch_size, len(compatible)))
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device_type="cuda", dtype=torch.bfloat16,
                                enabled=not args.no_amp):
                teacher_output, student_output, batch, ids = paired_forward(
                    teacher, student, samples, torch.device("cuda"), cache)
                imitation = supervised_output_loss(
                    student_output, samples, ids, torch.device("cuda"))
                transfer = distillation_loss(teacher_output, student_output,
                                             batch, ids, samples)
                loss = imitation + args.distill_weight * transfer
            if not torch.isfinite(loss):
                raise ValueError(f"nonfinite student loss at group {global_group} step {offset}")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(student.parameters(), 1.0)
            optimizer.step()
            losses[task].append(float(loss.detach().cpu()))
            task_examples[task] += len(samples)
        _save(args.output / "resume.pt", dict(
            spec_digest=spec_digest, model=student.state_dict(),
            optimizer=optimizer.state_dict(), python_rng=rng.getstate(),
            torch_rng=torch.get_rng_state(), cuda_rng=torch.cuda.get_rng_state_all(),
            task_steps=dict(task_steps), task_examples=dict(task_examples),
            next_group=global_group + 1))
        print(json.dumps(dict(stage="fit", group=global_group + 1,
                              groups=total_groups, games=info["games"],
                              samples=sum(map(len, buckets.values())),
                              cadence_action_windows=cadence_info["action_windows"],
                              elapsed_seconds=round(time.perf_counter() - started, 1),
                              loss={task: sum(values) / len(values)
                                    for task, values in losses.items()},
                              cache=cache.stats(),
                              gpu_peak_bytes=torch.cuda.max_memory_allocated())), flush=True)
        del buckets, cache, losses, samples, anchor, compatible
        gc.collect()
        groups_this_run += 1
        if (args.max_groups_this_run and groups_this_run >= args.max_groups_this_run
                and global_group + 1 < total_groups):
            return dict(status="paused", next_group=global_group + 1,
                        total_groups=total_groups)
    validation, validation_info = collect(
        args.validation_release, "validation",
        games_per_matchup=args.validation_games_per_matchup,
        history=args.history, bucket_limit=args.bucket_limit,
        per_game_bucket_limit=args.per_game_bucket_limit, seed=args.seed,
        quality_index=validation_quality)
    student.eval()
    validation_rows = []
    with torch.no_grad():
        for category in sorted(validation):
            for sample in validation[category]:
                loss, metrics = sample_loss(student, sample, torch.device("cuda"))
                validation_rows.append(dict(category="/".join(category),
                                            loss=float(loss.detach().cpu()),
                                            game_id=sample[4]["game_id"], **metrics))
    if not validation_rows:
        raise ValueError("empty student validation set")
    report = dict(schema=SCHEMA, training_ready=False, strength_validated=False,
                  deployment="none", spec_digest=spec_digest,
                  source_identity_sha256=train_sha,
                  validation_identity_sha256=validation_sha,
                  teacher_sha256=spec["teacher_sha256"],
                  teacher_family=spec["teacher_family"],
                  student_width=args.width, total_groups=total_groups,
                  training_steps_by_task=dict(task_steps),
                  training_examples_by_task=dict(task_examples),
                  training_action_observation="cadence",
                  validation_action_observation="before_command",
                  validation=validation_info,
                  balanced_validation=summarize_validation(validation_rows),
                  mean_validation_loss=sum(row["loss"] for row in validation_rows) /
                  len(validation_rows), gpu=torch.cuda.get_device_name(),
                  gpu_peak_bytes=torch.cuda.max_memory_allocated())
    _save(args.output / "student.pt", dict(
        schema="protodd-whole-game-fit-v1",
        state_dict={name: tensor.detach().cpu() for name, tensor in student.state_dict().items()},
        source_identity_sha256=train_sha, distillation_schema=SCHEMA,
        teacher_sha256=spec["teacher_sha256"]))
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                               encoding="utf-8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("teacher", "release", "validation_release", "quality_index",
                 "validation_quality_index", "output"):
        parser.add_argument("--" + name.replace("_", "-"), required=True, type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=160)
    parser.add_argument("--chunk-games-per-matchup", type=int, default=8)
    parser.add_argument("--validation-games-per-matchup", type=int, default=1)
    parser.add_argument("--collect-workers", type=int, default=2)
    parser.add_argument("--steps-per-chunk", type=int, default=256)
    parser.add_argument("--epochs", type=int, default=1)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--width", type=int, default=128)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--bucket-limit", type=int, default=64)
    parser.add_argument("--per-game-bucket-limit", type=int, default=2)
    parser.add_argument("--encoding-cache-mb", type=int, default=256)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--distill-weight", type=float, default=0.5)
    parser.add_argument("--high-mmr-threshold", type=int, default=2300)
    parser.add_argument("--high-mmr-share", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-groups-this-run", type=int, default=0)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--no-amp", action="store_true")
    args = parser.parse_args()
    if args.max_groups_this_run < 0:
        parser.error("--max-groups-this-run must be nonnegative")
    result = fit(args)
    print(json.dumps(result if result.get("status") == "paused" else
                     dict(stage="complete", output=str(args.output.resolve()),
                          mean_validation_loss=result["mean_validation_loss"])), flush=True)


if __name__ == "__main__":
    main()
