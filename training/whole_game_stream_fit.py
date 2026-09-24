"""Memory-bounded GPU teacher fit over a frozen, player-diverse replay cohort.

Only one small group of games is decoded at a time. Each completed group has
an atomic optimizer/RNG checkpoint so a long corpus run can resume exactly.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
import gc
import hashlib
import json
import os
from pathlib import Path
import multiprocessing
import random
import time

os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
import torch

from .whole_game_batch import minibatch_loss
from .whole_game_encoding_cache import ObservationCache
from .whole_game_fit import (SCHEMA as FIT_SCHEMA, MATCHUPS, choose_games, collect,
                             initialize_model, sample_loss, summarize_validation,
                             validate_release_pair)
from .whole_game_model import WholeGameModel
from .whole_game_quality import load_index
from .whole_game_release import key


SCHEMA = "protodd-whole-game-stream-fit-v1"
SOURCE_FILES = ("whole_game_stream_fit.py", "whole_game_fit.py", "whole_game_batch.py",
                "whole_game_model.py", "whole_game_features.py", "whole_game_quality.py",
                "whole_game_encoding_cache.py")


def _digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def plan_chunks(release, quality, *, games_per_matchup, chunk_games_per_matchup,
                seed, high_mmr_threshold, high_mmr_share):
    """Fix game identity/order before training; reject an incomplete requested cohort."""
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if record["split"] == "train" and record["matchup"] in MATCHUPS and \
                (release / "games" / key(record) / "receipt.json").exists():
            eligible[record["matchup"]].append(record)
    if any(len(eligible[matchup]) < games_per_matchup for matchup in MATCHUPS):
        raise ValueError("requested replay cohort is not fully extracted")
    selected, quality_counts = choose_games(eligible, "train", games_per_matchup, seed,
                                            quality, high_mmr_threshold, high_mmr_share)
    by_matchup = {}
    for matchup in MATCHUPS:
        ids = [record["game_id"] for record in eligible[matchup]
               if record["game_id"] in selected]
        ids.sort(key=lambda game_id: hashlib.sha256(
            f"{seed}:stream:{game_id}".encode()).digest())
        if len(ids) != games_per_matchup:
            raise ValueError("selection did not produce the requested cohort")
        by_matchup[matchup] = ids
    groups = []
    for offset in range(0, games_per_matchup, chunk_games_per_matchup):
        groups.append({matchup: by_matchup[matchup][offset:offset + chunk_games_per_matchup]
                       for matchup in MATCHUPS})
    return groups, quality_counts


def verify_chunks(release, groups, *, games_per_matchup, chunk_games_per_matchup):
    """Prevent a reused selection from importing test games or missing shards."""
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    training = {record["game_id"]: record for record in identity["selected"]
                if record["split"] == "train" and record["matchup"] in MATCHUPS}
    expected_groups = (games_per_matchup + chunk_games_per_matchup - 1) // chunk_games_per_matchup
    if len(groups) != expected_groups:
        raise ValueError("cohort group count changed")
    seen = set()
    for offset, group in enumerate(groups):
        expected_length = min(chunk_games_per_matchup,
                              games_per_matchup - offset * chunk_games_per_matchup)
        if set(group) != set(MATCHUPS):
            raise ValueError("cohort lacks a matchup")
        for matchup in MATCHUPS:
            if len(group[matchup]) != expected_length:
                raise ValueError("cohort group length changed")
            for game_id in group[matchup]:
                record = training.get(game_id)
                if (game_id in seen or record is None or record["matchup"] != matchup or
                        not (release / "games" / key(record) / "receipt.json").exists()):
                    raise ValueError("cohort includes a duplicate, wrong split or missing shard")
                seen.add(game_id)


def _category_keys(buckets):
    keys = sorted(buckets)
    result = dict(event=[key for key in keys if key[1] == "event"],
                  action=[key for key in keys if key[1] not in ("event", "forecast")],
                  forecast=[key for key in keys if key[1] == "forecast"])
    if any(not result[task] for task in result):
        raise ValueError("stream group lacks action, event or forecast supervision")
    return result


def _collect_matchup(payload):
    (release, quality_path, matchup, game_ids, games_per_matchup, history,
     bucket_limit, per_game_bucket_limit, seed, threshold, share) = payload
    quality = load_index(quality_path, Path(release) / "identity.json")
    return collect(release, "train", game_ids=set(game_ids),
                   required_matchups=(matchup,), games_per_matchup=games_per_matchup,
                   history=history, bucket_limit=bucket_limit,
                   per_game_bucket_limit=per_game_bucket_limit, seed=seed,
                   quality_index=quality, high_mmr_threshold=threshold,
                   high_mmr_share=share)


def _collect_group(args, group, quality, seed):
    selected_ids = {game_id for ids in group.values() for game_id in ids}
    if args.collect_workers == 1:
        return collect(args.release, "train", game_ids=selected_ids,
                       games_per_matchup=args.chunk_games_per_matchup,
                       history=args.history, bucket_limit=args.bucket_limit,
                       per_game_bucket_limit=args.per_game_bucket_limit, seed=seed,
                       quality_index=quality, high_mmr_threshold=args.high_mmr_threshold,
                       high_mmr_share=args.high_mmr_share)
    payloads = [(args.release, args.quality_index, matchup, group[matchup],
                 len(group[matchup]), args.history, args.bucket_limit,
                 args.per_game_bucket_limit, seed, args.high_mmr_threshold,
                 args.high_mmr_share) for matchup in MATCHUPS]
    with ProcessPoolExecutor(max_workers=args.collect_workers,
                             mp_context=multiprocessing.get_context("spawn")) as pool:
        results = list(pool.map(_collect_matchup, payloads))
    buckets = {}
    games = {}
    quality_selection = {}
    quality_samples = Counter()
    bucket_candidates = 0
    for shard, info in results:
        if set(shard) & set(buckets):
            raise ValueError("parallel collection produced duplicate category")
        buckets.update(shard)
        games.update(info["games"])
        quality_selection.update(info["quality_selection"])
        quality_samples.update(info["quality_samples"])
        bucket_candidates += info["bucket_candidates"]
    return buckets, dict(games=games, quality_selection=quality_selection,
                         quality_samples=dict(quality_samples),
                         bucket_candidates=bucket_candidates,
                         buckets={"/".join(key): len(value) for key, value in buckets.items()})


def _atomic_save(path, value):
    temporary = path.with_suffix(".tmp")
    torch.save(value, temporary)
    os.replace(temporary, path)


def fit(args):
    torch.use_deterministic_algorithms(True)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cuda.matmul.allow_tf32 = False
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required")
    if (args.games_per_matchup < 1 or args.chunk_games_per_matchup < 1 or
            args.steps_per_chunk < 1 or args.epochs < 1 or args.width < 64 or
            args.batch_size < 1 or args.encoding_cache_mb < 1 or
            args.learning_rate <= 0 or args.validation_games_per_matchup < 1 or
            args.collect_workers not in (1, 2, 3)):
        raise ValueError("invalid streaming fit limits")
    train_identity_sha, validation_identity_sha = validate_release_pair(
        args.release, args.validation_release)
    quality = load_index(args.quality_index, args.release / "identity.json")
    validation_quality = load_index(args.validation_quality_index,
                                    args.validation_release / "identity.json")
    inherited = None
    if args.resume:
        inherited = json.loads((args.output / "run.json").read_text(encoding="utf8"))
    elif args.selection_from:
        inherited = json.loads(args.selection_from.read_text(encoding="utf8"))
    if inherited is None:
        groups, quality_counts = plan_chunks(
            args.release, quality, games_per_matchup=args.games_per_matchup,
            chunk_games_per_matchup=args.chunk_games_per_matchup, seed=args.seed,
            high_mmr_threshold=args.high_mmr_threshold,
            high_mmr_share=args.high_mmr_share)
    else:
        if (inherited["source_identity_sha256"] != train_identity_sha or
                inherited["quality_index_sha256"] != _digest(args.quality_index) or
                inherited["games_per_matchup"] != args.games_per_matchup or
                inherited["chunk_games_per_matchup"] != args.chunk_games_per_matchup):
            raise ValueError("inherited replay cohort does not match this fit")
        groups, quality_counts = inherited["groups"], inherited["quality_selection"]
        verify_chunks(args.release, groups, games_per_matchup=args.games_per_matchup,
                      chunk_games_per_matchup=args.chunk_games_per_matchup)
    sources = {name: _digest(Path(__file__).parent / name) for name in SOURCE_FILES}
    tasks = args.task_schedule.split(",")
    if set(tasks) != {"action", "event", "forecast"} or not tasks:
        raise ValueError("schedule must cover action, event and forecast")
    spec = dict(schema=SCHEMA, source_identity_sha256=train_identity_sha,
                validation_identity_sha256=validation_identity_sha,
                quality_index_sha256=_digest(args.quality_index),
                validation_quality_index_sha256=_digest(args.validation_quality_index),
                source_code_sha256=sources, groups=groups,
                quality_selection=quality_counts, games_per_matchup=args.games_per_matchup,
                chunk_games_per_matchup=args.chunk_games_per_matchup,
                validation_games_per_matchup=args.validation_games_per_matchup,
                epochs=args.epochs, steps_per_chunk=args.steps_per_chunk,
                batch_size=args.batch_size, width=args.width, history=args.history,
                bucket_limit=args.bucket_limit, per_game_bucket_limit=args.per_game_bucket_limit,
                learning_rate=args.learning_rate, task_schedule=tasks,
                seed=args.seed, encoding_cache_mb=args.encoding_cache_mb,
                collect_workers=args.collect_workers,
                deterministic_algorithms=True,
                high_mmr_threshold=args.high_mmr_threshold,
                high_mmr_share=args.high_mmr_share, bf16_amp=not args.no_amp,
                initialization_checkpoint_sha256=_digest(args.init_checkpoint)
                if args.init_checkpoint else None)
    spec_digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    output = args.output
    if args.resume:
        prior = json.loads((output / "run.json").read_text(encoding="utf8"))
        if prior != spec:
            raise ValueError("resume configuration or source changed")
    else:
        if output.exists():
            raise FileExistsError(output)
        output.mkdir(parents=True)
        (output / "run.json").write_text(json.dumps(spec, indent=2) + "\n", encoding="utf8")
    device = torch.device("cuda")
    model = WholeGameModel(width=args.width).to(device).train()
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate)
    randomizer = random.Random(args.seed)
    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)
    task_steps = Counter()
    task_examples = Counter()
    next_group = 0
    if args.resume:
        saved = torch.load(output / "resume.pt", map_location="cpu", weights_only=True)
        if saved["spec_digest"] != spec_digest:
            raise ValueError("checkpoint belongs to another cohort")
        model.load_state_dict(saved["model"])
        optimizer.load_state_dict(saved["optimizer"])
        randomizer.setstate(saved["python_rng"])
        torch.set_rng_state(saved["torch_rng"].cpu())
        torch.cuda.set_rng_state_all(saved["cuda_rng"])
        task_steps.update(saved["task_steps"])
        task_examples.update(saved["task_examples"])
        next_group = saved["next_group"]
    elif args.init_checkpoint:
        initialize_model(model, args.init_checkpoint, train_identity_sha)
    total_groups = args.epochs * len(groups)
    started = time.perf_counter()
    groups_this_run = 0
    for global_group in range(next_group, total_groups):
        epoch, slot = divmod(global_group, len(groups))
        order = list(range(len(groups)))
        random.Random(args.seed + epoch).shuffle(order)
        group = groups[order[slot]]
        buckets, info = _collect_group(args, group, quality, args.seed + epoch)
        if any(info["games"].get(matchup, 0) != len(group[matchup]) for matchup in MATCHUPS):
            raise ValueError("stream group changed while fitting")
        category_keys = _category_keys(buckets)
        cache = ObservationCache(args.encoding_cache_mb * 1024 * 1024)
        losses = defaultdict(list)
        for offset in range(args.steps_per_chunk):
            global_step = global_group * args.steps_per_chunk + offset
            task = tasks[global_step % len(tasks)]
            category = category_keys[task][task_steps[task] % len(category_keys[task])]
            task_steps[task] += 1
            anchor = randomizer.choice(buckets[category])
            compatible = [sample for sample in buckets[category]
                          if sample[3].shape == anchor[3].shape]
            samples = randomizer.sample(compatible, min(args.batch_size, len(compatible)))
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device_type="cuda", dtype=torch.bfloat16, enabled=not args.no_amp):
                loss = minibatch_loss(model, samples, device, cache)
            if not torch.isfinite(loss):
                raise ValueError(f"nonfinite loss at step {global_step}")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            losses[task].append(float(loss.detach().cpu()))
            task_examples[task] += len(samples)
        _atomic_save(output / "resume.pt", dict(
            spec_digest=spec_digest, model=model.state_dict(), optimizer=optimizer.state_dict(),
            python_rng=randomizer.getstate(), torch_rng=torch.get_rng_state(),
            cuda_rng=torch.cuda.get_rng_state_all(), task_steps=dict(task_steps),
            task_examples=dict(task_examples), next_group=global_group + 1))
        print(json.dumps(dict(stage="fit", group=global_group + 1,
                              groups=total_groups, games=info["games"],
                              samples=sum(map(len, buckets.values())),
                              elapsed_seconds=round(time.perf_counter() - started, 1),
                              loss={task: sum(values) / len(values) for task, values in losses.items()},
                              cache=cache.stats(), gpu_peak_bytes=torch.cuda.max_memory_allocated())),
              flush=True)
        del buckets, cache, losses, samples, anchor, compatible
        gc.collect()
        groups_this_run += 1
        if (args.max_groups_this_run and groups_this_run >= args.max_groups_this_run
                and global_group + 1 < total_groups):
            return dict(status="paused", next_group=global_group + 1,
                        groups=total_groups, checkpoint=str((output / "resume.pt").resolve()))
    validation, validation_info = collect(
        args.validation_release, "validation", games_per_matchup=args.validation_games_per_matchup,
        history=args.history, bucket_limit=args.bucket_limit,
        per_game_bucket_limit=args.per_game_bucket_limit, seed=args.seed,
        quality_index=validation_quality)
    model.eval()
    validation_rows = []
    with torch.no_grad():
        for category in sorted(validation):
            for sample in validation[category]:
                loss, metrics = sample_loss(model, sample, device)
                validation_rows.append(dict(category="/".join(category),
                                            loss=float(loss.detach().cpu()),
                                            game_id=sample[4]["game_id"],
                                            quality_band=sample[4]["quality_band"], **metrics))
    report = dict(schema=SCHEMA, training_ready=False, strength_validated=False,
                  deployment="none", source_identity_sha256=train_identity_sha,
                  validation_identity_sha256=validation_identity_sha,
                  source_code_sha256=sources, spec_digest=spec_digest,
                  training_games={matchup: args.games_per_matchup for matchup in MATCHUPS},
                  total_groups=total_groups, steps=total_groups * args.steps_per_chunk,
                  training_steps_by_task=dict(task_steps),
                  training_examples_by_task=dict(task_examples),
                  validation=validation_info, validation_rows=validation_rows,
                  balanced_validation=summarize_validation(validation_rows),
                  mean_validation_loss=sum(row["loss"] for row in validation_rows) / len(validation_rows),
                  gpu=torch.cuda.get_device_name(),
                  gpu_peak_bytes=torch.cuda.max_memory_allocated())
    _atomic_save(output / "teacher.pt", dict(
        state_dict={name: tensor.detach().cpu() for name, tensor in model.state_dict().items()},
        schema=FIT_SCHEMA, source_identity_sha256=train_identity_sha))
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--validation-release", type=Path, required=True)
    parser.add_argument("--quality-index", type=Path, required=True)
    parser.add_argument("--validation-quality-index", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--init-checkpoint", type=Path)
    parser.add_argument("--selection-from", type=Path,
                        help="Reuse the exact train cohort in another run.json")
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--max-groups-this-run", type=int, default=0,
                        help="Stop cleanly after this many checkpointed groups (0 means all)")
    parser.add_argument("--games-per-matchup", type=int, default=128)
    parser.add_argument("--chunk-games-per-matchup", type=int, default=8)
    parser.add_argument("--collect-workers", type=int, default=1,
                        help="Parallel matchup decoders (1-3); raises peak host RAM")
    parser.add_argument("--validation-games-per-matchup", type=int, default=1)
    parser.add_argument("--epochs", type=int, default=1)
    parser.add_argument("--steps-per-chunk", type=int, default=64)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--bucket-limit", type=int, default=64)
    parser.add_argument("--per-game-bucket-limit", type=int, default=2)
    parser.add_argument("--encoding-cache-mb", type=int, default=512)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--task-schedule", default="action,action,event,forecast")
    parser.add_argument("--high-mmr-threshold", type=int, default=2300)
    parser.add_argument("--high-mmr-share", type=float, default=0.5)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-amp", action="store_true")
    args = parser.parse_args()
    if args.max_groups_this_run < 0:
        parser.error("--max-groups-this-run must be nonnegative")
    result = fit(args)
    if result.get("status") == "paused":
        print(json.dumps(dict(stage="paused", **result)), flush=True)
        return
    print(json.dumps(dict(stage="complete", output=str(args.output.resolve()),
                          steps=result["steps"], mean_validation_loss=result["mean_validation_loss"])),
          flush=True)


if __name__ == "__main__":
    main()
