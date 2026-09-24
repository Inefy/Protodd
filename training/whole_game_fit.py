"""Experimental causal whole-game teacher fit on verified compressed shards.

This is an offline training stage. It does not export a tournament controller,
prove command completion, or validate game strength. Stratified action sampling
prevents plentiful right-clicks from hiding rare command families.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict, deque
import hashlib
import json
import math
from pathlib import Path
import random
import time

import torch
from torch.nn import functional as F

from .whole_game_features import encode_label, encode_observation, static_grid
from .whole_game_batch import minibatch_loss
from .whole_game_encoding_cache import ObservationCache
from .whole_game_future import derive
from .whole_game_model import (DOMAINS, FORECAST_BINARY_TARGETS, FORECAST_HORIZONS,
                               WholeGameModel, masked_action_loss)
from .whole_game_quality import load_index
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard

SCHEMA = "protodd-whole-game-fit-v1"
MATCHUPS = ("PvT", "PvZ", "PvP")


def _seed(seed, split, matchup):
    token = f"{seed}:{split}:{matchup}".encode()
    return int.from_bytes(hashlib.sha256(token).digest()[:8], "little")


def player_diverse(records, quality):
    """Preserve seeded order within each player while cycling across players."""
    by_player = defaultdict(deque)
    for record in records:
        by_player[quality[record["game_id"]]["actor_group"]].append(record)
    result = []
    active = list(by_player)
    while active:
        next_active = []
        for player in active:
            result.append(by_player[player].popleft())
            if by_player[player]:
                next_active.append(player)
        active = next_active
    return result


def merge_quality_buckets(tier_buckets, limit, high_share):
    """Bound each action category while retaining its intended quality mix."""
    if limit < 1 or not 0 <= high_share <= 1:
        raise ValueError("invalid quality bucket policy")
    categories = {category for category, _ in tier_buckets}
    merged = {}
    for category in categories:
        high = tier_buckets.get((category, "high"), [])
        base = tier_buckets.get((category, "base"), [])
        high_quota = min(len(high), math.ceil(limit * high_share))
        picked = high[:high_quota] + base[:limit - high_quota]
        if len(picked) < limit:
            picked += high[high_quota:high_quota + limit - len(picked)]
        merged[category] = picked
    return merged


def choose_games(eligible, split, games_per_matchup, seed, quality_index=None,
                 high_mmr_threshold=2300, high_mmr_share=0.5):
    """Pick distinct games; quality sampling applies to training only."""
    if not 0 <= high_mmr_share <= 1:
        raise ValueError("high_mmr_share must be between zero and one")
    chosen, quality_counts = set(), defaultdict(Counter)
    for matchup in MATCHUPS:
        ranked = sorted(eligible[matchup], key=lambda record:
                        hashlib.sha256(f"{seed}:{record['game_id']}".encode()).digest())
        if quality_index is not None and split == "train":
            quality = quality_index["games"]
            high = player_diverse([r for r in ranked if quality[r["game_id"]]["mmr_claim"]
                                   >= high_mmr_threshold], quality)
            base = player_diverse([r for r in ranked if quality[r["game_id"]]["mmr_claim"]
                                   < high_mmr_threshold], quality)
            high_slots = min(len(high), round(games_per_matchup * high_mmr_share))
            picked = high[:high_slots]
            picked += base[:games_per_matchup - len(picked)]
            if len(picked) < games_per_matchup:
                picked += high[high_slots:high_slots + games_per_matchup - len(picked)]
        else:
            picked = ranked[:games_per_matchup]
        chosen.update(r["game_id"] for r in picked)
        if quality_index is not None:
            players = set()
            for record in picked:
                entry = quality_index["games"][record["game_id"]]
                mmr = entry["mmr_claim"]
                players.add(entry["actor_group"])
                quality_counts[matchup]["selected"] += 1
                quality_counts[matchup]["mmr_ge_threshold"] += mmr >= high_mmr_threshold
            quality_counts[matchup]["distinct_actor_groups"] = len(players)
    return chosen, {matchup: dict(counts) for matchup, counts in quality_counts.items()}


def collect(release, split, *, games_per_matchup, history, bucket_limit, seed, forecast=True,
            quality_index=None, high_mmr_threshold=2300, high_mmr_share=0.5,
            per_game_bucket_limit=2, game_ids=None, required_matchups=MATCHUPS):
    """Stream bounded causal action, timing and future-state samples."""
    if history < 1 or bucket_limit < 1 or games_per_matchup < 1 or per_game_bucket_limit < 1:
        raise ValueError("sample limits must be positive")
    game_ids = set(game_ids) if game_ids is not None else None
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text())
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == split and record["matchup"] in MATCHUPS and
                (game_ids is None or record["game_id"] in game_ids) and
                (release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, quality_counts = choose_games(eligible, split, games_per_matchup, seed,
                                         quality_index, high_mmr_threshold, high_mmr_share)
    games = Counter()
    seen = Counter()
    buckets = defaultdict(list)
    for record, directory, _ in selected_shards(release, split, game_ids=chosen):
        matchup = record["matchup"]
        if matchup not in MATCHUPS or games[matchup] >= games_per_matchup:
            continue
        games[matchup] += 1
        terrain = static_grid(load_terrain(directory))
        recent = deque(maxlen=history)
        pending = {}
        recent_actions = deque()
        game_seen = Counter()
        game_buckets = defaultdict(list)
        rng = random.Random(_seed(seed, split, record["game_id"]))
        quality = quality_index["games"][record["game_id"]] if quality_index else None
        sample_meta = dict(game_id=record["game_id"],
                           mmr_claim=quality["mmr_claim"] if quality else None,
                           quality_band=("high" if quality["mmr_claim"] >= high_mmr_threshold else "base")
                           if quality else None)
        def offer(category, sample):
            sample = (*sample, sample_meta)
            game_seen[category] += 1
            bucket = game_buckets[category]
            if len(bucket) < per_game_bucket_limit:
                bucket.append(sample)
            else:
                pick = rng.randrange(game_seen[category])
                if pick < per_game_bucket_limit:
                    bucket[pick] = sample
        for row, target in trajectory_shard(directory):
            if target["update_memory"]:
                if forecast:
                    while recent_actions and recent_actions[0][0] < row["frame"] - max(FORECAST_HORIZONS):
                        recent_actions.popleft()
                    for horizon in FORECAST_HORIZONS:
                        previous = pending.get(row["frame"] - horizon)
                        if previous is None:
                            continue
                        context, source = previous
                        domains = {domain for when, domain in recent_actions
                                   if source["frame"] <= when < row["frame"]}
                        future = derive(source, row, domains)
                        signature = "".join(str(int(value)) for value in (
                            future["newly_explored_tiles"] > 0,
                            future["newly_known_enemy_ids_retained"] > 0,
                            future["own_hp_shield_loss_common"] > 0,
                            future["technology_completed_gain"] + future["upgrade_level_gain"] > 0))
                        category = (matchup, "forecast", str(horizon), signature)
                        offer(category, (context, source, dict(forecast=future, horizon=horizon), terrain))
                    pending[row["frame"]] = (list(recent), row)
                    pending.pop(row["frame"] - max(FORECAST_HORIZONS) - 24, None)
                if target["event"] is not None:
                    category = (matchup, "event", str(target["event"]))
                    offer(category, (list(recent), row, target, terrain))
                recent.append(row)
            elif target["action"] is not None:
                label = target["action"]
                recent_actions.append((row["frame"], label["domain"]))
                category = (matchup, label["domain"], label["actions"]["kind"])
                offer(category, (list(recent), row, target, terrain))
        # Each game offers at most bucket_limit examples to the global
        # reservoir, so a long replay cannot monopolize a command family.
        for category, offered in game_buckets.items():
            for sample in offered:
                tier = sample[4]["quality_band"] if quality_index and split == "train" else None
                reservoir_key = (category, tier) if tier else category
                seen[reservoir_key] += 1
                bucket = buckets[reservoir_key]
                if len(bucket) < bucket_limit:
                    bucket.append(sample)
                else:
                    pick = rng.randrange(seen[reservoir_key])
                    if pick < bucket_limit:
                        bucket[pick] = sample
    if quality_index and split == "train":
        buckets = merge_quality_buckets(buckets, bucket_limit, high_mmr_share)
    if not all(games[m] for m in required_matchups):
        raise ValueError(f"{split} needs at least one completed shard in each required matchup")
    return buckets, dict(games=dict(games), quality_selection=quality_counts,
                         bucket_candidates=sum(seen.values()),
                         quality_samples=dict(Counter(sample[4]["quality_band"]
                             for samples in buckets.values() for sample in samples)),
                         buckets={"/".join(k): len(v) for k, v in buckets.items()})


def _on_device(value, device):
    return {name: tensor.to(device) if isinstance(tensor, torch.Tensor) else tensor
            for name, tensor in value.items()}


def sample_loss(model, sample, device):
    context, row, supervision, terrain = sample[:4]
    memory = None
    for prior in context:
        batch, _, _ = encode_observation(prior, terrain)
        memory = model(_on_device(batch, device), memory)["memory"]
    batch, ids, overflow = encode_observation(row, terrain)
    output = model(_on_device(batch, device), memory)
    if "forecast" in supervision:
        future = supervision["forecast"]
        horizon = supervision["horizon"]
        truth = [int(future["newly_explored_tiles"] > 0),
                 int(future["newly_known_enemy_ids_retained"] > 0),
                 int(future["own_hp_shield_loss_common"] > 0),
                 int(future["technology_completed_gain"] + future["upgrade_level_gain"] > 0),
                 int(future["visible_enemies_at_target"] > 0),
                 *future["confirmed_command_domains"]]
        if len(truth) != len(FORECAST_BINARY_TARGETS):
            raise ValueError("forecast target width changed")
        prediction = output[f"forecast_{horizon}"]
        binary = torch.tensor([truth], dtype=torch.float32, device=device)
        magnitude = math.log1p(future["newly_explored_tiles"]) / math.log1p(len(row["vision"]))
        expected_magnitude = torch.tensor([magnitude], dtype=torch.float32, device=device)
        loss = F.binary_cross_entropy_with_logits(prediction[:, :-1], binary) + \
               F.smooth_l1_loss(prediction[:, -1], expected_magnitude)
        predicted = (torch.sigmoid(prediction[:, :-1]) >= 0.5).detach().cpu()[0].tolist()
        metrics = dict(task="forecast", horizon=horizon, binary_truth=truth,
                       binary_correct=[int(bool(p) == bool(t)) for p, t in zip(predicted, truth)],
                       exploration_magnitude_error=abs(float(prediction[:, -1].detach().cpu()[0]) - magnitude))
    elif supervision["update_memory"]:
        expected = torch.tensor([supervision["event"]], dtype=torch.float32, device=device)
        loss = F.binary_cross_entropy_with_logits(output["event"], expected)
        metrics = dict(task="event", positive=supervision["event"],
                       predicted=float(torch.sigmoid(output["event"]).detach().cpu()[0]))
    else:
        label = supervision["action"]
        encoded = _on_device(encode_label(label, ids, terrain.shape[1], terrain.shape[0]), device)
        loss, heads = masked_action_loss(output, encoded)
        metrics = dict(task="action", domain=label["domain"], kind=label["actions"]["kind"],
                       domain_correct=int(output["domain"].argmax(-1).item() == encoded["domain"].item()),
                       kind_correct=int(output["kind"].argmax(-1).item() == encoded["kind"].item()),
                       supervised_heads=sorted(heads))
    metrics["overflow"] = overflow
    return loss, metrics


def summarize_validation(rows):
    grouped = defaultdict(list)
    for row in rows:
        matchup = row["category"].split("/", 1)[0]
        group = ("event" if row["task"] == "event" else
                 f"forecast_{row['horizon']}" if row["task"] == "forecast" else row["domain"])
        grouped[(matchup, group)].append(row)
    result = {}
    for (matchup, group), items in sorted(grouped.items()):
        summary = dict(samples=len(items), mean_loss=sum(x["loss"] for x in items) / len(items),
                       overflow_samples=sum(x["overflow"] > 0 for x in items))
        if group == "event":
            summary.update(positive=sum(x["positive"] for x in items),
                           accuracy=sum((x["predicted"] >= 0.5) == bool(x["positive"]) for x in items) / len(items))
        elif group.startswith("forecast_"):
            summary.update(binary_accuracy={name: sum(x["binary_correct"][index] for x in items) / len(items)
                          for index, name in enumerate(FORECAST_BINARY_TARGETS)},
                           positive_rate={name: sum(x["binary_truth"][index] for x in items) / len(items)
                          for index, name in enumerate(FORECAST_BINARY_TARGETS)},
                           exploration_magnitude_mae=sum(x["exploration_magnitude_error"] for x in items) / len(items))
        else:
            summary.update(domain_accuracy=sum(x["domain_correct"] for x in items) / len(items),
                           kind_accuracy=sum(x["kind_correct"] for x in items) / len(items))
        result[f"{matchup}/{group}"] = summary
    return result


def validate_release_pair(train_release, validation_release):
    """Prove cross-release evaluation remains disjoint at game identity level."""
    train_release, validation_release = Path(train_release), Path(validation_release)
    train_identity = json.loads((train_release / "identity.json").read_text())
    validation_identity = json.loads((validation_release / "identity.json").read_text())
    train_ids = {row["game_id"] for row in train_identity["selected"] if row["split"] == "train"}
    validation_ids = {row["game_id"] for row in validation_identity["selected"]
                      if row["split"] == "validation"}
    if not train_ids or not validation_ids or train_ids & validation_ids:
        raise ValueError("training and validation game identities overlap or are empty")
    train_hashes = {row["replay_sha256"] for row in train_identity["selected"]
                    if row["split"] == "train"}
    validation_hashes = {row["replay_sha256"] for row in validation_identity["selected"]
                         if row["split"] == "validation"}
    if train_hashes & validation_hashes:
        raise ValueError("training and validation replay files overlap")
    return (hashlib.sha256((train_release / "identity.json").read_bytes()).hexdigest(),
            hashlib.sha256((validation_release / "identity.json").read_bytes()).hexdigest())


def initialize_model(model, checkpoint_path, train_identity_sha):
    """Continue only from a matching architecture and frozen training cohort."""
    checkpoint_path = Path(checkpoint_path)
    checkpoint_sha = hashlib.sha256(checkpoint_path.read_bytes()).hexdigest()
    previous = torch.load(checkpoint_path, map_location="cpu", weights_only=True)
    if previous.get("schema") != SCHEMA or previous.get("source_identity_sha256") != train_identity_sha:
        raise ValueError("initial checkpoint schema or training release differs")
    model.load_state_dict(previous["state_dict"], strict=True)
    return checkpoint_sha


def fit(args):
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required for this experimental teacher fit")
    if args.output.exists():
        raise FileExistsError(args.output)
    source_hashes = {name: hashlib.sha256((Path(__file__).parent / name).read_bytes()).hexdigest()
                     for name in ("whole_game_fit.py", "whole_game_batch.py", "whole_game_model.py",
                                  "whole_game_features.py", "whole_game_quality.py",
                                  "whole_game_encoding_cache.py")}
    validation_release = args.validation_release or args.release
    train_identity_sha, validation_identity_sha = validate_release_pair(args.release, validation_release)
    quality_index = load_index(args.quality_index, args.release / "identity.json") if args.quality_index else None
    validation_quality_path = args.validation_quality_index or (
        args.quality_index if validation_release == args.release else None)
    validation_quality = load_index(validation_quality_path, validation_release / "identity.json") if validation_quality_path else None
    print(json.dumps(dict(stage="collect", train_release=str(args.release),
                          validation_release=str(validation_release),
                          games_per_matchup=args.games_per_matchup)), flush=True)
    train, train_info = collect(args.release, "train", games_per_matchup=args.games_per_matchup,
                                history=args.history, bucket_limit=args.bucket_limit, seed=args.seed,
                                forecast=not args.no_forecast, quality_index=quality_index,
                                high_mmr_threshold=args.high_mmr_threshold,
                                high_mmr_share=args.high_mmr_share,
                                per_game_bucket_limit=args.per_game_bucket_limit)
    validation, validation_info = collect(validation_release, "validation", games_per_matchup=args.games_per_matchup,
                                          history=args.history, bucket_limit=args.bucket_limit, seed=args.seed,
                                          forecast=not args.no_forecast, quality_index=validation_quality,
                                          high_mmr_threshold=args.high_mmr_threshold,
                                          high_mmr_share=args.high_mmr_share,
                                          per_game_bucket_limit=args.per_game_bucket_limit)
    train_keys = sorted(train)
    val_keys = sorted(validation)
    if not any(k[1] not in ("event", "forecast") for k in train_keys) or not any(k[1] == "event" for k in train_keys):
        raise ValueError("training sample lacks actions or event timing")
    if not args.no_forecast and not any(k[1] == "forecast" for k in train_keys):
        raise ValueError("training sample lacks legal future-state windows")
    print(json.dumps(dict(stage="fit", train_games=train_info["games"],
                          validation_games=validation_info["games"],
                          train_samples=sum(map(len, train.values())),
                          validation_samples=sum(map(len, validation.values())),
                          steps=args.steps, batch_size=args.batch_size)), flush=True)
    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)
    randomizer = random.Random(args.seed)
    device = torch.device("cuda")
    model = WholeGameModel(width=args.width).to(device).train()
    initialization_sha = None
    if args.init_checkpoint:
        initialization_sha = initialize_model(model, args.init_checkpoint, train_identity_sha)
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate)
    train_losses = []
    task_keys = dict(event=[k for k in train_keys if k[1] == "event"],
                     action=[k for k in train_keys if k[1] not in ("event", "forecast")],
                     forecast=[k for k in train_keys if k[1] == "forecast"])
    tasks = args.task_schedule.split(",")
    if args.no_forecast and args.task_schedule == "event,action,forecast":
        tasks.remove("forecast")
    if not tasks or any(task not in task_keys or not task_keys[task] for task in tasks):
        raise ValueError("task schedule references an unavailable task")
    if set(tasks) != {task for task, keys in task_keys.items() if keys}:
        raise ValueError("task schedule must include every available task")
    args.output.mkdir(parents=True, exist_ok=False)
    task_steps = Counter()
    task_examples = Counter()
    task_losses = defaultdict(list)
    encoding_cache = ObservationCache(args.encoding_cache_mb * 1024 * 1024)
    fit_started = time.perf_counter()
    for step in range(args.steps):
        task = tasks[step % len(tasks)]
        category = task_keys[task][task_steps[task] % len(task_keys[task])]
        task_steps[task] += 1
        anchor = randomizer.choice(train[category])
        compatible = [sample for sample in train[category]
                      if sample[3].shape == anchor[3].shape]
        samples = randomizer.sample(compatible, min(args.batch_size, len(compatible)))
        optimizer.zero_grad(set_to_none=True)
        with torch.autocast(device_type="cuda", dtype=torch.bfloat16, enabled=not args.no_amp):
            loss = minibatch_loss(model, samples, device, encoding_cache)
        if not torch.isfinite(loss):
            raise ValueError(f"nonfinite loss at step {step}")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        train_losses.append(float(loss.detach().cpu()))
        task_losses[task].append(train_losses[-1])
        task_examples[task] += len(samples)
        if (step + 1) % args.progress_every == 0:
            print(json.dumps(dict(stage="fit", step=step + 1, steps=args.steps,
                                  elapsed_seconds=round(time.perf_counter() - fit_started, 1),
                                  task_steps=dict(task_steps),
                                  recent_loss={name: sum(values[-32:]) / len(values[-32:])
                                               for name, values in task_losses.items()},
                                  encoding_cache=encoding_cache.stats(),
                                  gpu_peak_bytes=torch.cuda.max_memory_allocated())), flush=True)
    model.eval()
    validation_rows = []
    with torch.no_grad():
        for category in val_keys:
            for sample in validation[category]:
                loss, metrics = sample_loss(model, sample, device)
                validation_rows.append(dict(category="/".join(category),
                                            loss=float(loss.detach().cpu()),
                                            game_id=sample[4]["game_id"],
                                            quality_band=sample[4]["quality_band"], **metrics))
    if not validation_rows:
        raise ValueError("empty held-out validation sample")
    report = dict(schema=SCHEMA, experimental=True, training_ready=False,
                  strength_validated=False, deployment="none",
                  source_release=str(args.release.resolve()),
                  source_code_sha256=source_hashes,
                  source_identity_sha256=train_identity_sha,
                  validation_release=str(validation_release.resolve()),
                  validation_identity_sha256=validation_identity_sha,
                  quality_index_sha256=hashlib.sha256(args.quality_index.read_bytes()).hexdigest() if args.quality_index else None,
                  validation_quality_index_sha256=hashlib.sha256(validation_quality_path.read_bytes()).hexdigest() if validation_quality_path else None,
                  high_mmr_threshold=args.high_mmr_threshold if quality_index else None,
                  high_mmr_share=args.high_mmr_share if quality_index else None,
                  training=train_info, validation=validation_info,
                  history=args.history, steps=args.steps, width=args.width,
                  initialization_checkpoint_sha256=initialization_sha,
                  task_schedule=tasks,
                  batch_size=args.batch_size, bf16_amp=not args.no_amp,
                  bucket_limit=args.bucket_limit,
                  per_game_bucket_limit=args.per_game_bucket_limit, seed=args.seed,
                  forecast_horizons=[] if args.no_forecast else list(FORECAST_HORIZONS),
                  training_steps_by_task=dict(task_steps),
                  training_examples_by_task=dict(task_examples),
                  encoding_cache=encoding_cache.stats(),
                  model_parameters=sum(p.numel() for p in model.parameters()),
                  gpu=torch.cuda.get_device_name(), gpu_peak_bytes=torch.cuda.max_memory_allocated(),
                  mean_train_loss=sum(train_losses) / len(train_losses),
                  mean_train_loss_by_task={task: sum(values) / len(values) for task, values in task_losses.items()},
                  mean_validation_loss=sum(r["loss"] for r in validation_rows) / len(validation_rows),
                  balanced_validation=summarize_validation(validation_rows),
                  validation_by_quality={band: summarize_validation([
                      row for row in validation_rows if row["quality_band"] == band])
                      for band in ("base", "high")} if validation_quality else None,
                  validation_rows=validation_rows)
    torch.save({"state_dict": model.cpu().state_dict(), "schema": SCHEMA,
                "source_identity_sha256": report["source_identity_sha256"]}, args.output / "teacher.pt")
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--validation-release", type=Path,
                        help="Separate frozen held-out release for an interim training fit")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--games-per-matchup", type=int, default=128)
    parser.add_argument("--history", type=int, default=8)
    parser.add_argument("--bucket-limit", type=int, default=64,
                        help="Global examples retained per matchup/action category")
    parser.add_argument("--per-game-bucket-limit", type=int, default=2,
                        help="Maximum examples each replay offers per action category")
    parser.add_argument("--steps", type=int, default=4096)
    parser.add_argument("--batch-size", type=int, default=16)
    parser.add_argument("--init-checkpoint", type=Path)
    parser.add_argument("--task-schedule", default="event,action,forecast",
                        help="Repeating task order, for example action,action,event,forecast")
    parser.add_argument("--progress-every", type=int, default=128)
    parser.add_argument("--encoding-cache-mb", type=int, default=512)
    parser.add_argument("--no-amp", action="store_true", help="Disable BF16 mixed precision")
    parser.add_argument("--width", type=int, default=256)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--no-forecast", action="store_true", help="Action/timing-only ablation")
    parser.add_argument("--quality-index", type=Path, help="Exact-player ladder MMR sidecar for train sampling")
    parser.add_argument("--validation-quality-index", type=Path,
                        help="Sidecar bound to a separate validation release")
    parser.add_argument("--high-mmr-threshold", type=int, default=2300)
    parser.add_argument("--high-mmr-share", type=float, default=0.5)
    args = parser.parse_args()
    if (args.steps < 1 or args.width < 64 or args.learning_rate <= 0 or
            args.games_per_matchup < 1 or args.bucket_limit < 1 or args.per_game_bucket_limit < 1 or
            args.batch_size < 1 or
            args.progress_every < 1 or
            args.encoding_cache_mb < 1 or
            args.high_mmr_threshold < 2000 or not 0 <= args.high_mmr_share <= 1):
        parser.error("positive steps, width >= 64 and positive learning rate required")
    result = fit(args)
    print(json.dumps(dict(schema=result["schema"], output=str(args.output.resolve()),
                          training_games=result["training"]["games"],
                          validation_games=result["validation"]["games"],
                          steps=result["steps"], training_steps_by_task=result["training_steps_by_task"],
                          mean_train_loss_by_task=result["mean_train_loss_by_task"],
                          mean_validation_loss=result["mean_validation_loss"],
                          gpu_peak_bytes=result["gpu_peak_bytes"]), indent=2))


if __name__ == "__main__":
    main()
