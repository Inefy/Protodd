"""Resumable GPU fit for the ordered six-command whole-game teacher.

This trains a causal multi-command head on verified train shards and evaluates
only disjoint validation shards. It is an offline fit, not a strength result.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
import os
from pathlib import Path
import random
import time

os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")
import torch

from .whole_game_encoding_cache import ObservationCache
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_multislot_batch import multislot_minibatch_loss
from .whole_game_multislot_collect import collect_multislot_windows
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_model import WholeGameModel
from .whole_game_quality import load_index
from .whole_game_release import key
from .whole_game_stream_fit import plan_chunks, verify_chunks


SCHEMA = "protodd-whole-game-multislot-fit-v1"
SOURCE_FILES = (
    "whole_game_multislot_fit.py", "whole_game_multislot_model.py",
    "whole_game_multislot_batch.py", "whole_game_multislot_collect.py",
    "whole_game_cadence_sequences.py", "whole_game_model.py",
    "whole_game_features.py", "whole_game_batch.py", "whole_game_structured_loss.py",
    "whole_game_action_schema.py", "whole_game_shards.py", "whole_game_sequences.py",
    "whole_game_stream_fit.py", "whole_game_fit.py", "whole_game_quality.py",
    "whole_game_encoding_cache.py",
)


def _digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _atomic_save(path, value):
    temporary = path.with_suffix(".tmp")
    torch.save(value, temporary)
    os.replace(temporary, path)


def _validation_group(release, games_per_matchup, seed):
    identity = json.loads((Path(release) / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "validation" and record["matchup"] in MATCHUPS and
                (Path(release) / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "validation", games_per_matchup, seed)
    group = {matchup: [record["game_id"] for record in eligible[matchup]
                       if record["game_id"] in chosen] for matchup in MATCHUPS}
    if any(len(group[matchup]) != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("validation cohort is incomplete")
    return group


def _initialize(model, checkpoint, identity_sha):
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved.get("source_identity_sha256") != identity_sha:
        raise ValueError("initial backbone belongs to another replay release")
    baseline = WholeGameModel(width=model.width,
                              mixture_components=model.mixture_components)
    baseline.load_state_dict(saved["state_dict"], strict=True)
    incompatible = model.load_state_dict(saved["state_dict"], strict=False)
    if (incompatible.unexpected_keys or set(incompatible.missing_keys) !=
            set(model.state_dict()) - set(baseline.state_dict())):
        raise ValueError("initial checkpoint is not an exact baseline backbone")
    def prefix(destination, source):
        with torch.no_grad():
            destination.weight.zero_()
            destination.weight[:, :source.in_features].copy_(source.weight)
            destination.bias.copy_(source.bias)

    # Slot zero starts at the validated one-command policy. Extra actor/kind
    # conditioning begins at zero and the ordered transition learns it.
    with torch.no_grad():
        model.slot_stop.weight.zero_()
        model.slot_stop.bias.zero_()
        model.slot_stop.weight[0].copy_(model.event.weight[0])
        model.slot_stop.bias[0].copy_(model.event.bias[0])
        model.slot_actor_key.load_state_dict(model.actor_key.state_dict())
        model.slot_actor_query.load_state_dict(model.actor_query.state_dict())
        model.slot_target_key.load_state_dict(model.target_key.state_dict())
    prefix(model.slot_kind, model.kind)
    prefix(model.slot_mode, model.target_mode)
    prefix(model.slot_domain, model.domain)
    prefix(model.slot_target_query, model.target_query)
    prefix(model.slot_position, model.position)
    prefix(model.slot_queued, model.queued)
    for name, head in model.slot_arguments.items():
        prefix(head, model.argument_heads[name])


def _pick_samples(buckets, counts, step, batch_size, rng):
    action_keys = [key for key in buckets if key[1] != "no_action"]
    stop_keys = [key for key in buckets if key[1] == "no_action"]
    if not action_keys or not stop_keys:
        raise ValueError("group requires both action and STOP windows")
    mode = ("rare", "natural", "rare", "natural", "stop")[step % 5]
    if mode == "stop":
        category = rng.choice(stop_keys)
    elif mode == "rare":
        category = rng.choice(action_keys)
    else:
        category = rng.choices(action_keys,
                               weights=[counts["/".join(key)] for key in action_keys])[0]
    anchor = rng.choice(buckets[category])
    compatible = [sample for sample in buckets[category]
                  if sample[3].shape == anchor[3].shape]
    return rng.sample(compatible, min(batch_size, len(compatible))), mode


def fit(args):
    if args.device == "cuda" and not torch.cuda.is_available():
        raise RuntimeError("CUDA unavailable")
    if (args.games_per_matchup < 1 or args.chunk_games_per_matchup < 1 or
            args.validation_games_per_matchup < 1 or args.steps_per_group < 1 or
            args.epochs < 1 or args.batch_size < 1 or args.width < 64 or
            args.history < 1 or args.category_limit < 1 or
            args.per_game_category_limit < 1 or args.learning_rate <= 0 or
            args.encoding_cache_mb < 1 or args.validation_limit_per_category < 1):
        raise ValueError("invalid fit limits")
    torch.use_deterministic_algorithms(True)
    torch.backends.cudnn.benchmark = False
    torch.backends.cudnn.deterministic = True
    torch.backends.cudnn.allow_tf32 = False
    torch.backends.cuda.matmul.allow_tf32 = False
    if args.device == "cpu":
        torch.set_num_threads(2)
    train_sha, validation_sha = validate_release_pair(
        args.release, args.validation_release)
    quality = load_index(args.quality_index, args.release / "identity.json") \
        if args.quality_index else None
    if args.resume:
        inherited = json.loads((args.output / "run.json").read_text(encoding="utf8"))
        groups = inherited["groups"]
        verify_chunks(args.release, groups, games_per_matchup=args.games_per_matchup,
                      chunk_games_per_matchup=args.chunk_games_per_matchup)
    elif args.selection_from:
        inherited = json.loads(args.selection_from.read_text(encoding="utf8"))
        if (inherited["source_identity_sha256"] != train_sha or
                inherited["games_per_matchup"] != args.games_per_matchup or
                inherited["chunk_games_per_matchup"] != args.chunk_games_per_matchup):
            raise ValueError("frozen selection differs from requested release/cohort")
        groups = inherited["groups"]
        verify_chunks(args.release, groups, games_per_matchup=args.games_per_matchup,
                      chunk_games_per_matchup=args.chunk_games_per_matchup)
    else:
        groups, _ = plan_chunks(args.release, quality,
                                games_per_matchup=args.games_per_matchup,
                                chunk_games_per_matchup=args.chunk_games_per_matchup,
                                seed=args.seed, high_mmr_threshold=args.high_mmr_threshold,
                                high_mmr_share=args.high_mmr_share)
    validation_group = _validation_group(args.validation_release,
                                         args.validation_games_per_matchup, args.seed)
    sources = {name: _digest(Path(__file__).parent / name) for name in SOURCE_FILES}
    spec = dict(schema=SCHEMA, training_identity_sha256=train_sha,
                validation_identity_sha256=validation_sha, groups=groups,
                validation_group=validation_group, source_code_sha256=sources,
                init_checkpoint_sha256=_digest(args.init_checkpoint)
                if args.init_checkpoint else None,
                quality_index_sha256=_digest(args.quality_index)
                if args.quality_index else None,
                games_per_matchup=args.games_per_matchup,
                chunk_games_per_matchup=args.chunk_games_per_matchup,
                validation_games_per_matchup=args.validation_games_per_matchup,
                epochs=args.epochs, steps_per_group=args.steps_per_group,
                batch_size=args.batch_size, width=args.width,
                mixture_components=args.mixture_components,
                maximum_slots=args.maximum_slots, history=args.history,
                category_limit=args.category_limit,
                per_game_category_limit=args.per_game_category_limit,
                learning_rate=args.learning_rate, seed=args.seed,
                device=args.device, amp=args.device == "cuda" and not args.no_amp,
                encoding_cache_mb=args.encoding_cache_mb,
                validation_limit_per_category=args.validation_limit_per_category,
                high_mmr_threshold=args.high_mmr_threshold,
                high_mmr_share=args.high_mmr_share)
    digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    if args.resume:
        if json.loads((args.output / "run.json").read_text(encoding="utf8")) != spec:
            raise ValueError("resume source or configuration changed")
    else:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.mkdir(parents=True)
        (args.output / "run.json").write_text(json.dumps(spec, indent=2) + "\n",
                                                encoding="utf8")
    device = torch.device(args.device)
    torch.manual_seed(args.seed)
    if args.device == "cuda":
        torch.cuda.manual_seed_all(args.seed)
    model = MultiSlotWholeGameModel(width=args.width,
                                    mixture_components=args.mixture_components,
                                    maximum_slots=args.maximum_slots).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate)
    rng = random.Random(args.seed)
    next_group = 0
    if args.resume:
        saved = torch.load(args.output / "resume.pt", map_location="cpu", weights_only=True)
        if saved["spec_digest"] != digest:
            raise ValueError("resume checkpoint belongs to another fit")
        model.load_state_dict(saved["model"])
        optimizer.load_state_dict(saved["optimizer"])
        rng.setstate(saved["python_rng"])
        torch.set_rng_state(saved["torch_rng"].cpu())
        if args.device == "cuda":
            torch.cuda.set_rng_state_all(saved["cuda_rng"])
        next_group = saved["next_group"]
    elif args.init_checkpoint:
        _initialize(model, args.init_checkpoint, train_sha)
    total_groups = args.epochs * len(groups)
    started = time.perf_counter()
    groups_this_run = 0
    for global_group in range(next_group, total_groups):
        epoch, slot = divmod(global_group, len(groups))
        order = list(range(len(groups)))
        random.Random(args.seed + epoch).shuffle(order)
        group = groups[order[slot]]
        buckets, info = collect_multislot_windows(
            args.release, "train", group, history=args.history,
            per_game_category_limit=args.per_game_category_limit,
            category_limit=args.category_limit, seed=args.seed + epoch)
        cache = ObservationCache(args.encoding_cache_mb * 1024 * 1024)
        losses = defaultdict(list)
        examples = Counter()
        model.train()
        for step in range(args.steps_per_group):
            samples, mode = _pick_samples(buckets, info["windows"],
                                          global_group * args.steps_per_group + step,
                                          args.batch_size, rng)
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device_type=args.device, dtype=torch.bfloat16,
                                enabled=args.device == "cuda" and not args.no_amp):
                loss, _ = multislot_minibatch_loss(
                    model, samples, device, cache, report=False)
            if not torch.isfinite(loss):
                raise ValueError("nonfinite multi-command loss")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            losses[mode].append(float(loss.detach().cpu()))
            examples[mode] += len(samples)
        _atomic_save(args.output / "resume.pt", dict(
            spec_digest=digest, model=model.state_dict(),
            optimizer=optimizer.state_dict(), python_rng=rng.getstate(),
            torch_rng=torch.get_rng_state(),
            cuda_rng=torch.cuda.get_rng_state_all() if args.device == "cuda" else [],
            next_group=global_group + 1))
        print(json.dumps(dict(stage="fit", group=global_group + 1,
                              groups=total_groups, elapsed_seconds=round(
                                  time.perf_counter() - started, 1), games=info["games"],
                              retained=sum(map(len, buckets.values())),
                              examples=dict(examples),
                              loss={name: sum(values) / len(values)
                                    for name, values in losses.items()},
                              gpu_peak_bytes=torch.cuda.max_memory_allocated()
                              if args.device == "cuda" else None)), flush=True)
        groups_this_run += 1
        if (args.max_groups_this_run and groups_this_run >= args.max_groups_this_run
                and global_group + 1 < total_groups):
            return dict(status="paused", next_group=global_group + 1,
                        groups=total_groups)
    validation, validation_info = collect_multislot_windows(
        args.validation_release, "validation", validation_group,
        history=args.history,
        per_game_category_limit=args.per_game_category_limit,
        category_limit=args.category_limit, seed=args.seed)
    model.eval()
    validation_rows = []
    with torch.no_grad():
        for category in sorted(validation):
            for sample in validation[category][:args.validation_limit_per_category]:
                loss, details = multislot_minibatch_loss(model, [sample], device)
                validation_rows.append(dict(category="/".join(category),
                                            game_id=sample[4]["game_id"],
                                            loss=float(loss.detach()), **details))
    report = dict(schema=SCHEMA, training_ready=False,
                  strength_validated=False, deployment="none",
                  spec_digest=digest, training_identity_sha256=train_sha,
                  validation_identity_sha256=validation_sha,
                  source_code_sha256=sources, total_groups=total_groups,
                  steps=total_groups * args.steps_per_group,
                  validation=validation_info, validation_rows=validation_rows,
                  mean_validation_loss=sum(row["loss"] for row in validation_rows) /
                  len(validation_rows), device=args.device,
                  gpu_peak_bytes=torch.cuda.max_memory_allocated()
                  if args.device == "cuda" else None)
    _atomic_save(args.output / "teacher.pt", dict(
        schema=SCHEMA, source_identity_sha256=train_sha,
        state_dict={name: tensor.detach().cpu()
                    for name, tensor in model.state_dict().items()}))
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                               encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "validation_release", "output"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    parser.add_argument("--quality-index", type=Path)
    parser.add_argument("--selection-from", type=Path)
    parser.add_argument("--init-checkpoint", type=Path)
    parser.add_argument("--resume", action="store_true")
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    parser.add_argument("--no-amp", action="store_true")
    parser.add_argument("--games-per-matchup", type=int, default=160)
    parser.add_argument("--chunk-games-per-matchup", type=int, default=8)
    parser.add_argument("--validation-games-per-matchup", type=int, default=8)
    parser.add_argument("--epochs", type=int, default=3)
    parser.add_argument("--steps-per-group", type=int, default=128)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--mixture-components", type=int, default=12)
    parser.add_argument("--maximum-slots", type=int, default=6)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--category-limit", type=int, default=64)
    parser.add_argument("--per-game-category-limit", type=int, default=2)
    parser.add_argument("--validation-limit-per-category", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--high-mmr-threshold", type=int, default=2300)
    parser.add_argument("--high-mmr-share", type=float, default=0.5)
    parser.add_argument("--encoding-cache-mb", type=int, default=512)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-groups-this-run", type=int, default=0)
    args = parser.parse_args()
    print(json.dumps(fit(args), indent=2), flush=True)


if __name__ == "__main__":
    main()
