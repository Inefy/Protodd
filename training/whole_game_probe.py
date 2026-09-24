"""Run a bounded GPU gradient probe on training-only v3.2 action labels.

This verifies tensor and loss plumbing. It is intentionally neither a strength
experiment nor a tournament checkpoint; the v3.2 pilot remains unqualified for
full training until live parity, outcome and rare-action gates pass.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import torch
from torch.nn import functional as F

from .whole_game_features import encode_label, encode_observation, static_grid
from .whole_game_model import WholeGameModel, masked_action_loss
from .whole_game_pilot import SCHEMA
from .whole_game_sequences import trajectory


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def run(pilot, output):
    pilot, output = Path(pilot), Path(output)
    report = json.loads((pilot / "report.json").read_text())
    if not report.get("complete") or report["schema"] != SCHEMA or report.get("training_ready"):
        raise ValueError("expected complete training-only experimental pilot")
    if not torch.cuda.is_available():
        raise RuntimeError("CUDA is required for the local GPU probe")
    samples = []
    for index, game in enumerate(report["reports"]):
        if game["split"] != "train":
            raise ValueError("probe may consume training split only")
        source = pilot / f"{index:02d}-{game['matchup']}"
        grid = static_grid(json.loads((source / "extracted/terrain.json").read_text()))
        labels = {}
        with (source / "imitation-labels.jsonl").open() as stream:
            for line in stream:
                label = json.loads(line)
                labels[label["observation_sequence"]] = label
        chosen = set()
        with (source / "extracted/observations.jsonl").open() as stream:
            for line in stream:
                row = json.loads(line)
                label = labels.get(row["sequence"])
                if label is None or row["reason"] != "before_command":
                    continue
                kind = label["actions"]["kind"]
                if kind in chosen:
                    continue
                batch, ids, overflow = encode_observation(row, grid)
                target = encode_label(label, ids, grid.shape[1], grid.shape[0])
                samples.append((game["matchup"], kind, batch, target, overflow))
                chosen.add(kind)
                if len(chosen) >= 8:
                    break
    if not samples:
        raise ValueError("no usable action targets")
    output.mkdir(parents=True, exist_ok=False)
    torch.manual_seed(42)
    torch.cuda.manual_seed_all(42)
    model = WholeGameModel(width=256).cuda().train()
    optimizer = torch.optim.AdamW(model.parameters(), lr=3e-4)
    losses = []
    for matchup, kind, batch, label, overflow in samples:
        batch = {key: value.cuda() for key, value in batch.items()}
        label = {key: value.cuda() if isinstance(value, torch.Tensor) else value
                 for key, value in label.items()}
        optimizer.zero_grad(set_to_none=True)
        prediction = model(batch)
        loss, heads = masked_action_loss(prediction, label)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite whole-game loss")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        losses.append(dict(matchup=matchup, kind=kind, loss=float(loss.detach().cpu()),
                           heads={key: float(value.cpu()) for key, value in heads.items()},
                           overflow=overflow))
    timing_steps = timing_positive = timing_updates = 0
    for index, game in enumerate(report["reports"]):
        source = pilot / f"{index:02d}-{game['matchup']}"
        grid = static_grid(json.loads((source / "extracted/terrain.json").read_text()))
        memory = None
        timing_losses = []
        for row, supervision in trajectory(source / "extracted", source / "imitation-labels.jsonl"):
            if not supervision["update_memory"] or supervision["event"] is None:
                continue
            batch, _, _ = encode_observation(row, grid)
            prediction = model({key: value.cuda() for key, value in batch.items()}, memory)
            memory = prediction["memory"]
            expected = torch.tensor([supervision["event"]], dtype=torch.float32, device="cuda")
            timing_losses.append(F.binary_cross_entropy_with_logits(prediction["event"], expected))
            timing_steps += 1
            timing_positive += supervision["event"]
            if len(timing_losses) == 8:
                optimizer.zero_grad(set_to_none=True)
                torch.stack(timing_losses).mean().backward()
                torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
                optimizer.step()
                timing_updates += 1
                memory = memory.detach()
                timing_losses.clear()
            if timing_steps >= 96 or timing_steps % 32 == 0:
                break
        if timing_losses:
            optimizer.zero_grad(set_to_none=True)
            torch.stack(timing_losses).mean().backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
            optimizer.step()
            timing_updates += 1
        if timing_steps >= 96:
            break
    result = dict(schema=SCHEMA, pilot=str(pilot.resolve()), pilot_report_sha256=digest(pilot / "report.json"),
                  model_source_sha256=digest(Path(__file__).with_name("whole_game_model.py")),
                  device=torch.cuda.get_device_name(), model_parameters=sum(p.numel() for p in model.parameters()),
                  gpu_peak_bytes=torch.cuda.max_memory_allocated(), steps=len(losses), samples=losses,
                  timing_steps=timing_steps, timing_positive=timing_positive, timing_updates=timing_updates,
                  experimental_probe=True, training_ready=False, strength_validated=False, deployment="none")
    (output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    print(json.dumps(run(**vars(parser.parse_args())), indent=2))


if __name__ == "__main__":
    main()
