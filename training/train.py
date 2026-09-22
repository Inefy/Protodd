"""Train macro imitation from validated datasets; final test games are never read here."""
import argparse
from collections import defaultdict
import json
import math
import os
from pathlib import Path
import random
import subprocess
import time

os.environ.setdefault("CUBLAS_WORKSPACE_CONFIG", ":4096:8")

import numpy as np
import torch
from torch.nn import functional as F

from .dataset import batches, metadata, samples
from .model import FrequencyBaseline, MacroModel, export_model, load_model, masked_logits
from .schema import load_schema, sha256


def evaluate(model, dataset, split, batch_size=256, device="cpu"):
    model.eval()
    action_names = load_schema()["actions"]
    totals = defaultdict(lambda: [0, 0.0, 0, 0, 0.0])
    game_totals = defaultdict(lambda: [0, 0.0, 0, 0, 0.0])
    with torch.no_grad():
        for features, masks, labels, weights, matchups, game_ids in batches(dataset, split, batch_size, device):
            raw = model(features)
            if not torch.isfinite(raw).all():
                raise ValueError("nonfinite evaluation logits")
            logits = masked_logits(raw, masks)
            loss = F.cross_entropy(logits, labels, reduction="none")
            prediction = logits.argmax(1)
            correct = prediction == labels
            top3 = (logits.topk(min(3, logits.shape[1]), dim=1).indices == labels[:, None]).any(1)
            illegal = ~masks.gather(1, raw.argmax(1)[:, None]).squeeze(1)
            probabilities = logits.softmax(1)
            brier = ((probabilities - F.one_hot(labels, logits.shape[1])) ** 2).sum(1)
            values = zip(loss.cpu().tolist(), correct.cpu().tolist(), top3.cpu().tolist(),
                         illegal.cpu().tolist(), brier.cpu().tolist(), matchups, game_ids, labels.cpu().tolist())
            for ce, hit, hit3, bad, score, matchup, game_id, label in values:
                keys = ["all", matchup, "wait" if label == 0 else "macro_actions"]
                if label != 0:
                    keys.append("action/" + action_names[label])
                for key in keys:
                    row = totals[key]
                    row[0] += 1; row[1] += ce; row[2] += int(hit); row[3] += int(hit3); row[4] += score
                row = game_totals[game_id]
                row[0] += 1; row[1] += ce; row[2] += int(hit); row[3] += int(hit3); row[4] += score
                totals["unmasked_illegal"][0] += int(bad)
    count = totals["all"][0]
    if not count:
        raise ValueError(f"no samples in {split}")
    report = {key: dict(samples=r[0], cross_entropy=r[1] / r[0], top1=r[2] / r[0],
                        top3=r[3] / r[0], brier=r[4] / r[0])
              for key, r in totals.items() if key != "unmasked_illegal"}
    report["unmasked_illegal_rate"] = totals["unmasked_illegal"][0] / count
    report["game_balanced"] = dict(games=len(game_totals))
    for index, key in enumerate(("cross_entropy", "top1", "top3", "brier"), 1):
        report["game_balanced"][key] = sum(r[index] / r[0] for r in game_totals.values()) / len(game_totals)
    return report


def train(dataset, output, hidden=(1024, 1024), epochs=20, batch_size=256,
          learning_rate=3e-4, seed=42, device="auto", patience=4):
    if epochs < 1 or batch_size < 1 or patience < 1 or not math.isfinite(learning_rate) or learning_rate <= 0:
        raise ValueError("invalid training configuration")
    info = metadata(dataset)
    if device == "auto":
        device = "cuda" if torch.cuda.is_available() else "cpu"
    if device not in ("cpu", "cuda") or (device == "cuda" and not torch.cuda.is_available()):
        raise ValueError("requested device is unavailable")
    random.seed(seed); np.random.seed(seed); torch.manual_seed(seed)
    torch.use_deterministic_algorithms(True)
    torch.set_num_threads(min(4, os.cpu_count() or 1))
    model = MacroModel(tuple(hidden)).to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=learning_rate, weight_decay=1e-4)
    output = Path(output)
    output.mkdir(parents=True, exist_ok=False)
    frequencies = [0.0] * len(load_schema()["actions"])
    for _, _, action, weight, _, _ in samples(dataset, "train"):
        frequencies[action] += weight
    baseline = evaluate(FrequencyBaseline(frequencies), dataset, "validation", batch_size, "cpu")
    config = dict(hidden=list(hidden), epochs=epochs, batch_size=batch_size, learning_rate=learning_rate,
                  seed=seed, device=device, patience=patience, torch=torch.__version__, numpy=np.__version__,
                  parameters=sum(p.numel() for p in model.parameters()), source=info["source"],
                  dataset_sha256=sha256(dataset), data_manifest_sha256=info["manifest_sha256"],
                  schema=load_schema(), synthetic_only=info["source"] == "synthetic_test")
    revision = subprocess.run(["git", "rev-parse", "HEAD"], capture_output=True, text=True, check=False)
    config["source_commit"] = revision.stdout.strip() if revision.returncode == 0 else None
    config["working_tree_dirty"] = bool(subprocess.run(
        ["git", "status", "--porcelain"], capture_output=True, text=True, check=False).stdout.strip())
    root = Path(__file__).resolve().parents[1]
    source_files = list((root / "training").glob("*.py")) + [
        root / "training/schema_v2.json", root / "training/requirements.txt",
        root / "include/protodd/ObservationEncoder.hpp", root / "src/core/ObservationEncoder.cpp",
        root / "include/protodd/LearnedPolicy.hpp", root / "src/core/LearnedPolicy.cpp"]
    config["source_hashes"] = {p.relative_to(root).as_posix(): sha256(p) for p in sorted(source_files)}
    (output / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    best, stale, history = math.inf, 0, []
    start = time.monotonic()
    for epoch in range(epochs):
        model.train()
        weighted_loss, total_weight = 0.0, 0.0
        for features, masks, labels, weights, _, _ in batches(dataset, "train", batch_size, device, seed + epoch):
            optimizer.zero_grad(set_to_none=True)
            logits = masked_logits(model(features), masks)
            loss = (F.cross_entropy(logits, labels, reduction="none") * weights).sum() / weights.sum()
            if not torch.isfinite(loss):
                raise ValueError("nonfinite training loss")
            loss.backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0, error_if_nonfinite=True)
            optimizer.step()
            batch_weight = weights.sum().item()
            weighted_loss += loss.item() * batch_weight
            total_weight += batch_weight
        if not total_weight:
            raise ValueError("no training samples")
        validation = evaluate(model, dataset, "validation", batch_size, device)
        score = validation["game_balanced"]["cross_entropy"]
        record = dict(epoch=epoch + 1, train_loss=weighted_loss / total_weight,
                      validation=validation, elapsed_seconds=time.monotonic() - start)
        history.append(record)
        print(json.dumps(record), flush=True)
        if score < best:
            best, stale = score, 0
            temporary = output / "best.bin.partial"
            export_model(model, temporary)
            temporary.replace(output / "LearnedMacro.bin")
        else:
            stale += 1
        (output / "history.json").write_text(json.dumps(history, indent=2) + "\n", encoding="utf-8")
        if stale >= patience:
            break
    exported = load_model(output / "LearnedMacro.bin")
    report = evaluate(exported, dataset, "validation", batch_size, "cpu")
    manifest = dict(config=config, model_sha256=sha256(output / "LearnedMacro.bin"),
                    validation=report, frequency_baseline=baseline, best_validation_cross_entropy=best,
                    epochs_completed=len(history), deployment="shadow-only", final_test_evaluated=False,
                    strength_validated=False)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--hidden", type=int, nargs=2, default=[1024, 1024])
    parser.add_argument("--epochs", type=int, default=20)
    parser.add_argument("--batch-size", type=int, default=256)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--device", choices=["auto", "cpu", "cuda"], default="auto")
    parser.add_argument("--patience", type=int, default=4)
    args = parser.parse_args()
    train(**vars(args))


if __name__ == "__main__":
    main()
