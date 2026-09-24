"""Resumable GPU experiment for replay-supervised map positions.

Uses the frozen whole-game teacher and the *training* groups from a source-pinned
stream fit. The observed actor/kind are supplied to the position head, making
this an architecture test, not a tournament controller.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import random

import torch

from .whole_game_fit import collect, validate_release_pair
from .whole_game_model import WholeGameModel
from .whole_game_quality import load_index
from .whole_game_spatial_head import SpatialPositionHead
from .whole_game_spatial_probe import _batch, _evaluate, _position_examples
from .whole_game_stream_fit import verify_chunks


SCHEMA = "protodd-whole-game-spatial-stream-v1"
SOURCES = ("whole_game_spatial_stream_fit.py", "whole_game_spatial_probe.py",
           "whole_game_action_schema.py",
           "whole_game_spatial_head.py", "whole_game_fit.py", "whole_game_features.py",
           "whole_game_model.py", "whole_game_quality.py", "whole_game_stream_fit.py")


def _sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def _atomic_save(path, obj):
    temporary = path.with_suffix(".tmp")
    torch.save(obj, temporary)
    os.replace(temporary, path)


def _spec(args, train_sha, validation_sha, selection):
    source_dir = Path(__file__).parent
    return dict(schema=SCHEMA, train_identity_sha256=train_sha,
                validation_identity_sha256=validation_sha,
                teacher_sha256=_sha(args.teacher),
                train_quality_sha256=_sha(args.quality_index),
                validation_quality_sha256=_sha(args.validation_quality_index),
                selection_sha256=_sha(args.selection),
                source_code_sha256={name: _sha(source_dir / name) for name in SOURCES},
                groups=selection["groups"], grid=args.grid, key_width=args.key_width,
                steps_per_group=args.steps_per_group, batch_size=args.batch_size,
                learning_rate=args.learning_rate, seed=args.seed)


def fit(args):
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required")
    if (args.grid < 2 or args.key_width < 1 or args.steps_per_group < 1 or
            args.batch_size < 1 or args.learning_rate <= 0 or args.max_groups_this_run < 0):
        raise ValueError("invalid spatial stream limits")
    train_sha, validation_sha = validate_release_pair(args.release, args.validation_release)
    selection = json.loads(args.selection.read_text(encoding="utf8"))
    if selection["source_identity_sha256"] != train_sha:
        raise ValueError("selection belongs to another release")
    verify_chunks(args.release, selection["groups"],
                  games_per_matchup=selection["games_per_matchup"],
                  chunk_games_per_matchup=selection["chunk_games_per_matchup"])
    source = torch.load(args.teacher, map_location="cpu", weights_only=True)
    if source.get("source_identity_sha256") != train_sha:
        raise ValueError("teacher and training release differ")
    spec = _spec(args, train_sha, validation_sha, selection)
    digest = hashlib.sha256(json.dumps(spec, sort_keys=True).encode()).hexdigest()
    args.output.mkdir(parents=True, exist_ok=args.resume)
    spec_path = args.output / "run.json"
    if args.resume:
        recorded = json.loads(spec_path.read_text(encoding="utf8"))
        if recorded["digest"] != digest:
            raise ValueError("resume configuration or source changed")
    else:
        spec_path.write_text(json.dumps(dict(digest=digest, **spec), indent=2) + "\n",
                             encoding="utf8")
    torch.set_num_threads(4)
    torch.use_deterministic_algorithms(True)
    torch.backends.cudnn.benchmark = False
    torch.manual_seed(args.seed)
    teacher = WholeGameModel(
        width=source["state_dict"]["memory.weight_hh"].shape[1],
        mixture_components=source["state_dict"]["position.weight"].shape[0] // 5)
    teacher.load_state_dict(source["state_dict"], strict=True)
    teacher.cuda().eval()
    for parameter in teacher.parameters():
        parameter.requires_grad_(False)
    head = SpatialPositionHead(state_width=teacher.width, grid=args.grid,
                               key_width=args.key_width, spatial_channels=7).cuda()
    optimizer = torch.optim.AdamW(head.parameters(), lr=args.learning_rate)
    rng = random.Random(args.seed)
    pool = []
    completed = 0
    groups_seen = []
    if args.resume:
        saved = torch.load(args.output / "resume.pt", map_location="cpu", weights_only=True)
        if saved["digest"] != digest:
            raise ValueError("checkpoint configuration changed")
        head.load_state_dict(saved["head"])
        optimizer.load_state_dict(saved["optimizer"])
        rng.setstate(saved["python_rng"])
        torch.set_rng_state(saved["torch_rng"])
        torch.cuda.set_rng_state_all(saved["cuda_rng"])
        pool, completed, groups_seen = saved["pool"], saved["completed"], saved["groups_seen"]
    quality = load_index(args.quality_index, args.release / "identity.json")
    validation_quality = load_index(args.validation_quality_index,
                                    args.validation_release / "identity.json")
    groups_this_run = 0
    for index in range(completed, len(selection["groups"])):
        group = selection["groups"][index]
        game_ids = {game_id for ids in group.values() for game_id in ids}
        buckets, info = collect(args.release, "train", game_ids=game_ids,
                                games_per_matchup=len(group["PvT"]), history=8,
                                bucket_limit=32, per_game_bucket_limit=2,
                                seed=args.seed, forecast=False, quality_index=quality)
        if info["games"] != {matchup: len(ids) for matchup, ids in group.items()}:
            raise ValueError("training group did not decode its exact frozen cohort")
        added = _position_examples(teacher, buckets, args.grid, entity_channels=True)
        if not added:
            raise ValueError("training group lacks position labels")
        pool.extend(added)
        head.train()
        losses = []
        for _ in range(args.steps_per_group):
            rows = rng.sample(pool, min(args.batch_size, len(pool)))
            state, spatial, actor_xy, kind, target = _batch(rows, torch.device("cuda"))
            optimizer.zero_grad(set_to_none=True)
            with torch.autocast(device_type="cuda", dtype=torch.bfloat16):
                prediction = head(state, spatial, actor_xy, kind)
                loss = head.loss(prediction, target)
            if not torch.isfinite(loss):
                raise ValueError("nonfinite spatial loss")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
            optimizer.step()
            losses.append(float(loss.detach().cpu()))
        groups_seen.append(dict(index=index + 1, games=info["games"],
                                position_samples=len(added), pool_samples=len(pool),
                                mean_loss=sum(losses) / len(losses)))
        completed = index + 1
        _atomic_save(args.output / "resume.pt", dict(
            digest=digest, head=head.state_dict(), optimizer=optimizer.state_dict(),
            python_rng=rng.getstate(), torch_rng=torch.get_rng_state(),
            cuda_rng=torch.cuda.get_rng_state_all(), pool=pool,
            completed=completed, groups_seen=groups_seen))
        print(json.dumps(dict(stage="fit", **groups_seen[-1])), flush=True)
        groups_this_run += 1
        if args.max_groups_this_run and groups_this_run >= args.max_groups_this_run:
            return dict(stage="paused", completed=completed,
                        total_groups=len(selection["groups"]))
    validation, validation_info = collect(
        args.validation_release, "validation", games_per_matchup=1,
        history=8, bucket_limit=64, per_game_bucket_limit=2,
        seed=args.seed, forecast=False, quality_index=validation_quality)
    heldout = _position_examples(teacher, validation, args.grid, entity_channels=True)
    if not heldout:
        raise ValueError("validation has no position labels")
    report = dict(schema=SCHEMA, oracle_actor_and_kind=True,
                  spatial_channels=("explored", "visible", "walkability",
                                    "own", "visible_enemy", "remembered_enemy", "neutral"),
                  tournament_ready=False, source_digest=digest,
                  training_games={matchup: sum(len(group[matchup]) for group in selection["groups"])
                                  for matchup in ("PvT", "PvZ", "PvP")},
                  validation_games=validation_info["games"],
                  training_samples=len(pool), validation_samples=len(heldout),
                  steps=len(selection["groups"]) * args.steps_per_group,
                  groups=groups_seen,
                  train=_evaluate(head, pool, torch.device("cuda")),
                  validation=_evaluate(head, heldout, torch.device("cuda")),
                  validation_predicted_context=_evaluate(
                      head, heldout, torch.device("cuda"), predicted_context=True),
                  gpu=torch.cuda.get_device_name(),
                  gpu_peak_bytes=torch.cuda.max_memory_allocated())
    _atomic_save(args.output / "head.pt", dict(
        schema=SCHEMA, source_digest=digest,
        oracle_actor_and_kind=True,
        state_dict={name: tensor.detach().cpu() for name, tensor in head.state_dict().items()}))
    (args.output / "report.json").write_text(json.dumps(report, indent=2) + "\n",
                                             encoding="utf8")
    return dict(stage="complete", completed=completed,
                validation_within_64px=report["validation"]["spatial_within_64px"],
                validation_samples=len(heldout))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("teacher", "release", "validation_release", "quality_index",
                 "validation_quality_index", "selection", "output"):
        parser.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    parser.add_argument("--grid", type=int, default=16)
    parser.add_argument("--key-width", type=int, default=64)
    parser.add_argument("--steps-per-group", type=int, default=128)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--learning-rate", type=float, default=1e-4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-groups-this-run", type=int, default=0)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    print(json.dumps(fit(args)), flush=True)


if __name__ == "__main__":
    main()
