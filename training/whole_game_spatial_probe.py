"""Bounded replay test of an actor-conditioned spatial position head.

The shared teacher is frozen. Training supplies the observed actor and command
kind, so this measures position-head potential, not end-to-end control.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import random
import statistics

import torch
from torch.nn import functional as F

from .whole_game_action_schema import supported_pair_mask
from .whole_game_features import encode_label, encode_observation
from .whole_game_fit import collect, validate_release_pair
from .whole_game_model import WholeGameModel
from .whole_game_quality import load_index
from .whole_game_spatial_head import SpatialPositionHead, entity_raster


SCHEMA = "protodd-whole-game-spatial-probe-v1"


def _position_examples(model, buckets, grid, *, entity_channels=False):
    examples = []
    device = next(model.parameters()).device
    with torch.no_grad():
        for category, samples in sorted(buckets.items()):
            if category[1] == "event":
                continue
            for sample in samples:
                context, row, supervision, terrain = sample[:4]
                label = supervision["action"]
                if not label["loss_masks"]["target_position"]:
                    continue
                memory = None
                for previous in context:
                    prior, _, _ = encode_observation(previous, terrain)
                    memory = model({name: value.to(device) for name, value in prior.items()},
                                   memory)["memory"]
                batch, ids, _ = encode_observation(row, terrain)
                encoded = encode_label(label, ids, terrain.shape[1], terrain.shape[0])
                if not encoded["mask"]["target_position"]:
                    continue
                positives = [ids[entity_id] for entity_id in label["actor_positive"]]
                if not positives:
                    continue
                output = model({name: value.to(device) for name, value in batch.items()}, memory)
                actor_xy = batch["entity_numeric"][0, positives, :2].mean(dim=0)
                own = batch["relation"][0].to(device) == 0
                predicted_actor = int(output["actor"][0].masked_fill(~own, -torch.inf).argmax())
                pair = output["kind"][0, :, None] + output["target_mode"][0, None, :]
                legal = supported_pair_mask(device)
                predicted_pair = int(pair.masked_fill(~legal, -torch.inf).flatten().argmax())
                predicted_kind = torch.tensor(predicted_pair // legal.shape[1])
                predicted_mode = predicted_pair % legal.shape[1]
                mixture = output["position"][0]
                centers = mixture[:, 1:3].sigmoid()
                target = encoded["target_position"][0].to(device)
                pixels = torch.tensor([terrain.shape[1] * 32, terrain.shape[0] * 32], device=device)
                errors = torch.linalg.vector_norm((centers - target) * pixels, dim=-1)
                selected = mixture[:, 0].argmax().item()
                pooled = F.adaptive_avg_pool2d(batch["spatial"], (grid, grid))
                if entity_channels:
                    pooled = torch.cat((pooled, entity_raster(batch, grid)), dim=1)
                examples.append(dict(state=output["memory"][0].cpu(),
                                     spatial=pooled[0].cpu(),
                                     actor_xy=actor_xy.cpu(), kind=encoded["kind"][0].cpu(),
                                     predicted_actor_xy=batch["entity_numeric"][0, predicted_actor, :2].cpu(),
                                     predicted_kind=predicted_kind,
                                     actor_correct=predicted_actor in positives,
                                     kind_correct=bool(predicted_kind == encoded["kind"][0]),
                                     mode_correct=predicted_mode == int(encoded["target_mode"][0]),
                                     target=target.cpu(), pixels=pixels.cpu(),
                                     baseline_error_px=float(errors[selected]),
                                     baseline_oracle_error_px=float(errors.min()),
                                     game_id=sample[4]["game_id"],
                                     kind_name=label["actions"]["kind"]))
    return examples


def _batch(examples, device, *, predicted_context=False):
    actor_key = "predicted_actor_xy" if predicted_context else "actor_xy"
    kind_key = "predicted_kind" if predicted_context else "kind"
    return (torch.stack([row["state"] for row in examples]).to(device),
            torch.stack([row["spatial"] for row in examples]).to(device),
            torch.stack([row[actor_key] for row in examples]).to(device),
            torch.stack([row[kind_key] for row in examples]).to(device),
            torch.stack([row["target"] for row in examples]).to(device))


def _evaluate(head, examples, device, *, predicted_context=False):
    rows = []
    head.eval()
    with torch.no_grad():
        for example in examples:
            state, spatial, actor_xy, kind, target = _batch(
                [example], device, predicted_context=predicted_context)
            prediction = head(state, spatial, actor_xy, kind)
            coordinate = head.decode(prediction)[0].cpu()
            error = float(torch.linalg.vector_norm((coordinate - example["target"]) *
                                                     example["pixels"]))
            rows.append(dict(game_id=example["game_id"], kind=example["kind_name"],
                             actor_correct=example["actor_correct"],
                             kind_correct=example["kind_correct"],
                             mode_correct=example["mode_correct"],
                             spatial_error_px=error,
                             gaussian_error_px=example["baseline_error_px"],
                             gaussian_oracle_error_px=example["baseline_oracle_error_px"]))
    return dict(samples=len(rows), predicted_context=predicted_context,
                actor_correct=sum(r["actor_correct"] for r in rows),
                kind_correct=sum(r["kind_correct"] for r in rows),
                mode_correct=sum(r["mode_correct"] for r in rows),
                actor_and_kind_correct=sum(r["actor_correct"] and r["kind_correct"] for r in rows),
                spatial_within_64px=sum(r["spatial_error_px"] <= 64 for r in rows),
                gaussian_within_64px=sum(r["gaussian_error_px"] <= 64 for r in rows),
                gaussian_oracle_within_64px=sum(r["gaussian_oracle_error_px"] <= 64 for r in rows),
                spatial_median_error_px=statistics.median(r["spatial_error_px"] for r in rows),
                gaussian_median_error_px=statistics.median(r["gaussian_error_px"] for r in rows),
                rows=rows)


def probe(checkpoint, release, validation_release, train_quality, validation_quality,
          selection, output, *, games_per_matchup=8, steps=512, batch_size=32, seed=42):
    checkpoint, release, validation_release = map(Path, (checkpoint, release, validation_release))
    output = Path(output)
    if output.exists() or games_per_matchup < 1 or steps < 1 or batch_size < 1:
        raise ValueError("existing output or invalid probe limits")
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA GPU required for spatial probe")
    train_sha, validation_sha = validate_release_pair(release, validation_release)
    source = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if source.get("source_identity_sha256") != train_sha:
        raise ValueError("teacher and training release differ")
    frozen = json.loads(Path(selection).read_text(encoding="utf8"))
    if frozen["source_identity_sha256"] != train_sha:
        raise ValueError("selection belongs to another release")
    chosen = {game_id for ids in frozen["groups"][0].values()
              for game_id in ids[:games_per_matchup]}
    if len(chosen) != 3 * games_per_matchup:
        raise ValueError("frozen group lacks requested games")
    quality = load_index(train_quality, release / "identity.json")
    validation_index = load_index(validation_quality, validation_release / "identity.json")
    torch.set_num_threads(4)
    torch.manual_seed(seed)
    model = WholeGameModel(width=source["state_dict"]["memory.weight_hh"].shape[1],
                           mixture_components=source["state_dict"]["position.weight"].shape[0] // 5)
    model.load_state_dict(source["state_dict"], strict=True)
    model.cuda().eval()
    train_buckets, train_selection = collect(
        release, "train", games_per_matchup=games_per_matchup, game_ids=chosen,
        history=8, bucket_limit=32, per_game_bucket_limit=2, seed=seed,
        forecast=False, quality_index=quality)
    validation_buckets, validation_selection = collect(
        validation_release, "validation", games_per_matchup=1,
        history=8, bucket_limit=64, per_game_bucket_limit=2, seed=seed,
        forecast=False, quality_index=validation_index)
    head = SpatialPositionHead(state_width=model.width).cuda()
    train = _position_examples(model, train_buckets, head.grid)
    validation = _position_examples(model, validation_buckets, head.grid)
    if not train or not validation:
        raise ValueError("position supervision missing")
    optimizer = torch.optim.AdamW(head.parameters(), lr=1e-3)
    rng = random.Random(seed)
    head.train()
    recent = []
    for step in range(steps):
        chosen_rows = rng.sample(train, min(batch_size, len(train)))
        state, spatial, actor_xy, kind, target = _batch(chosen_rows, torch.device("cuda"))
        optimizer.zero_grad(set_to_none=True)
        with torch.autocast(device_type="cuda", dtype=torch.bfloat16):
            prediction = head(state, spatial, actor_xy, kind)
            loss = head.loss(prediction, target)
        if not torch.isfinite(loss):
            raise ValueError(f"nonfinite spatial loss at step {step}")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(head.parameters(), 1.0)
        optimizer.step()
        recent.append(float(loss.detach().cpu()))
    train_score = _evaluate(head, train, torch.device("cuda"))
    validation_score = _evaluate(head, validation, torch.device("cuda"))
    predicted_validation_score = _evaluate(
        head, validation, torch.device("cuda"), predicted_context=True)
    output.mkdir(parents=True)
    torch.save(dict(schema=SCHEMA, state_dict={name: value.cpu() for name, value in head.state_dict().items()},
                    teacher_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                    train_identity_sha256=train_sha, validation_identity_sha256=validation_sha,
                    oracle_actor_and_kind=True), output / "head.pt")
    report = dict(schema=SCHEMA, oracle_actor_and_kind=True, tournament_ready=False,
                  source_code_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
                  teacher_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                  training_games=train_selection["games"], validation_games=validation_selection["games"],
                  train_samples=len(train), validation_samples=len(validation), steps=steps,
                  last_64_train_loss=sum(recent[-64:]) / min(64, len(recent)),
                  train=train_score, validation=validation_score,
                  validation_predicted_context=predicted_validation_score)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "validation_release", "train_quality",
                 "validation_quality", "selection", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=8)
    parser.add_argument("--steps", type=int, default=512)
    parser.add_argument("--batch-size", type=int, default=32)
    args = parser.parse_args()
    report = probe(args.checkpoint, args.release, args.validation_release,
                   args.train_quality, args.validation_quality, args.selection, args.output,
                   games_per_matchup=args.games_per_matchup, steps=args.steps,
                   batch_size=args.batch_size)
    print(json.dumps(dict(train_samples=report["train_samples"],
                          validation_samples=report["validation_samples"],
                          train={key: value for key, value in report["train"].items() if key != "rows"},
                          validation={key: value for key, value in report["validation"].items()
                                      if key != "rows"}), indent=2))


if __name__ == "__main__":
    main()
