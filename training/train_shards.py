"""Explicit local training on frozen replay tensors, with exact consumed-step resume.

No test split or live deployment is available through this entry point. Epochs
are sample-equivalent draws with replacement, balanced by matchup/game/player.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
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

from .model import FrequencyBaseline, MacroModel, export_model, load_model
from .schema import load_schema, sha256
from .shard_batches import BalancedRows, CudaTrainingCache, prefetched, tensor_batch
from .shards import TensorShards, positive_integer

ROOT = Path(__file__).resolve().parents[1]
DEFAULTS = dict(device="cuda", precision="bf16", hidden=[1024, 1024], seed=42,
                epochs=20, batch_size=4096, validation_batch_size=8192,
                learning_rate=3e-4, weight_decay=1e-4, warmup_steps=500,
                patience=4, prefetch=2, checkpoint_steps=250, status_seconds=15,
                cpu_threads=4, steps_per_epoch=None, allow_synthetic=False,
                parity_tool=None, data_cache="auto")


def atomic_json(path, value):
    path = Path(path)
    temporary = path.with_name(path.name + ".partial")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    os.replace(temporary, path)


class Status:
    def __init__(self, output, seconds):
        self.path = output / "status.json"
        self.seconds, self.last = seconds, 0.0
        self.state = dict(pid=os.getpid(), training_started=False, strength_validated=False,
                          deployment="shadow-only", final_test_evaluated=False)

    def update(self, force=False, **values):
        self.state.update(values)
        now = time.monotonic()
        if force or now - self.last >= self.seconds:
            self.state["updated_at"] = datetime.now(timezone.utc).isoformat()
            atomic_json(self.path, self.state)
            print(json.dumps(self.state, allow_nan=False), flush=True)
            self.last = now


def normalize_config(raw):
    if set(raw) - (set(DEFAULTS) | {"dataset", "output"}):
        raise ValueError("unknown trainer configuration fields")
    cfg = {**DEFAULTS, **raw}
    for key in ("dataset", "output"):
        cfg[key] = str(Path(cfg[key]).resolve())
    for key in ("epochs", "batch_size", "validation_batch_size", "patience", "prefetch",
                "checkpoint_steps", "status_seconds", "cpu_threads"):
        positive_integer(cfg[key], key)
    for key in ("seed", "warmup_steps"):
        positive_integer(cfg[key], key, 0)
    if cfg["steps_per_epoch"] is not None:
        positive_integer(cfg["steps_per_epoch"], "steps per epoch")
    for key in ("learning_rate", "weight_decay"):
        if not math.isfinite(cfg[key]) or cfg[key] < 0 or (key == "learning_rate" and cfg[key] == 0):
            raise ValueError(f"invalid {key}")
    if cfg["device"] not in ("cuda", "cpu") or cfg["precision"] not in ("fp32", "bf16"):
        raise ValueError("unsupported device/precision")
    if cfg["precision"] == "bf16" and cfg["device"] != "cuda":
        raise ValueError("BF16 training requires CUDA")
    if cfg["data_cache"] not in ("auto", "cuda", "stream"):
        raise ValueError("unsupported data cache")
    if cfg["data_cache"] == "cuda" and (cfg["device"] != "cuda" or cfg["precision"] != "bf16"):
        raise ValueError("GPU input caching requires CUDA BF16 autocast")
    if type(cfg["allow_synthetic"]) is not bool:
        raise ValueError("allow_synthetic must be boolean")
    if cfg["parity_tool"]:
        cfg["parity_tool"] = str(Path(cfg["parity_tool"]).resolve())
        if not Path(cfg["parity_tool"]).is_file():
            raise ValueError("missing C++ model parity tool")
    return cfg


def source_identity():
    names = ["training/train_shards.py", "training/shard_batches.py", "training/shards.py",
             "training/model.py", "training/schema.py", "training/schema_v2.json"]
    return {name: sha256(ROOT / name) for name in names}


def freeze_sources(output, identity):
    """Keep resume runnable even while the repository's trainer evolves."""
    snapshot = output / "source"
    for name, digest in identity["source_hashes"].items():
        target = snapshot / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((ROOT / name).read_bytes())
        if sha256(target) != digest:
            raise ValueError("trainer source changed during snapshot")
    (snapshot / "training" / "__init__.py").write_text("", encoding="utf-8")
    (snapshot / "resume.py").write_text(
        '"""Resume this run using its frozen trainer and original configuration."""\n'
        'import json\nfrom pathlib import Path\n'
        'from training.train_shards import train\n'
        'if __name__ == "__main__":\n'
        '    saved = json.loads((Path(__file__).resolve().parents[1] / "config.json").read_text())\n'
        '    train(saved["config"], resume=True)\n', encoding="utf-8")


def split_indices(dataset, split):
    if split not in ("train", "validation"):
        raise ValueError("final test cannot be opened by this trainer")
    return np.concatenate([np.arange(s["start"], s["start"] + s["count"], dtype=np.int64)
                           for s in dataset.sequences if s["split"] == split])


def training_prior(dataset):
    """Frequency control under exactly the training sampler's distribution."""
    games = defaultdict(set)
    perspectives = Counter(s["game_index"] for s in dataset.sequences if s["split"] == "train")
    for s in dataset.sequences:
        if s["split"] == "train":
            games[s["matchup"]].add(s["game_index"])
    coefficients = np.array([
        1.0 / (len(games) * len(games[s["matchup"]]) * perspectives[s["game_index"]] * s["count"])
        if s["split"] == "train" else 0.0 for s in dataset.sequences])
    starts = np.array([s["start"] for s in dataset.sequences])
    frequencies = np.zeros(len(load_schema()["actions"]), dtype=np.float64)
    for start, meta in dataset.metadata_blocks():
        seqs = np.searchsorted(starts, np.arange(start, start + len(meta)), side="right") - 1
        frequencies += np.bincount(meta["labels"], weights=meta["weights"] * coefficients[seqs],
                                   minlength=len(frequencies))
    # FrequencyBaseline adds 1e-3 pseudocounts; normalize to a sample-count scale.
    return frequencies / frequencies.sum() * sum(s["count"] for s in dataset.sequences if s["split"] == "train")


METRICS = ("cross_entropy", "top1", "top3", "brier", "unmasked_illegal_rate")


def summarize(totals):
    count = float(totals[0])
    return dict(samples=int(count), **{key: float(totals[i + 1] / count)
                                     for i, key in enumerate(METRICS)}) if count else dict(samples=0)


@torch.no_grad()
def evaluate(model, dataset, indices, config, status=None, phase="validation"):
    """Every validation row, FP32 inference, without truncation or test access."""
    model.eval()
    actions = load_schema()["actions"]
    game_totals = np.zeros((len(dataset.games), 6), dtype=np.float64)
    action_totals = np.zeros((len(actions), 6), dtype=np.float64)
    batch_size, device = config["validation_batch_size"], config["device"]
    def read(start):
        return tensor_batch(dataset.read_indices(indices[start:start + batch_size]), len(actions), device == "cuda")
    processed = 0
    for _, batch in prefetched(range(0, len(indices), batch_size), read, config["prefetch"]):
        features, masks, labels, _, games = (t.to(device, non_blocking=True) for t in batch)
        raw = model(features).float()
        if not torch.isfinite(raw).all():
            raise ValueError("nonfinite validation logits")
        logits = raw.masked_fill(~masks, -torch.inf)
        probabilities = logits.softmax(1)
        loss = F.cross_entropy(logits, labels, reduction="none")
        hit = logits.argmax(1) == labels
        top3 = (logits.topk(min(3, len(actions)), dim=1).indices == labels[:, None]).any(1)
        brier = (probabilities ** 2).sum(1) - 2 * probabilities.gather(1, labels[:, None]).squeeze(1) + 1
        illegal = ~masks.gather(1, raw.argmax(1)[:, None]).squeeze(1)
        values = torch.stack((torch.ones_like(loss), loss, hit.float(), top3.float(), brier, illegal.float()), 1).cpu().numpy()
        gids, label_array = batch[4].numpy(), batch[2].numpy()
        for col in range(6):
            game_totals[:, col] += np.bincount(gids, weights=values[:, col], minlength=len(dataset.games))
            action_totals[:, col] += np.bincount(label_array, weights=values[:, col], minlength=len(actions))
        processed += len(features)
        if status:
            status.update(phase=phase, validation_rows_processed=processed, validation_rows_total=len(indices))
    by_matchup = {}
    matchup_game_metrics = []
    present = game_totals[:, 0] > 0
    per_game = game_totals[present, 1:] / game_totals[present, :1]
    for matchup in sorted({g["matchup"] for g in dataset.games if g["split"] == "validation"}):
        keep = np.array([g["split"] == "validation" and g["matchup"] == matchup for g in dataset.games]) & present
        if not keep.any():
            continue
        means = (game_totals[keep, 1:] / game_totals[keep, :1]).mean(0)
        by_matchup[matchup] = dict(**summarize(game_totals[keep].sum(0)), games=int(keep.sum()),
                                   game_balanced={k: float(means[i]) for i, k in enumerate(METRICS)})
        matchup_game_metrics.append(means)
    balanced = np.mean(matchup_game_metrics, axis=0)
    return dict(all=summarize(action_totals.sum(0)), wait=summarize(action_totals[0]),
                macro_actions=summarize(action_totals[1:].sum(0)), by_matchup=by_matchup,
                per_action={a: summarize(action_totals[i]) for i, a in enumerate(actions)},
                game_balanced={k: float(per_game[:, i].mean()) for i, k in enumerate(METRICS)},
                matchup_balanced_game={k: float(balanced[i]) for i, k in enumerate(METRICS)},
                precision="fp32", final_test_evaluated=False)


def learning_rate(config, step, total):
    warmup = min(config["warmup_steps"], max(0, total - 1))
    if step < warmup:
        return config["learning_rate"] * (step + 1) / warmup
    progress = (step - warmup) / max(1, total - warmup - 1)
    return config["learning_rate"] * (0.1 + 0.9 * (1 + math.cos(math.pi * progress)) / 2)


def atomic_checkpoint(path, payload):
    temporary = path.with_name(path.name + ".partial")
    torch.save(payload, temporary)
    os.replace(temporary, path)


def rng_state():
    return dict(python=random.getstate(), numpy=np.random.get_state(), torch=torch.get_rng_state(),
                cuda=torch.cuda.get_rng_state_all() if torch.cuda.is_available() else [])


def restore_rng(state):
    random.setstate(state["python"])
    np.random.set_state(state["numpy"])
    torch.set_rng_state(state["torch"].cpu())
    if state["cuda"]:
        torch.cuda.set_rng_state_all([s.cpu() for s in state["cuda"]])


def acquire_lock(output):
    stream = (output / "run.lock").open("a+b")
    if stream.tell() == 0:
        stream.write(b"0"); stream.flush()
    stream.seek(0)
    try:
        if os.name == "nt":
            import msvcrt
            msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            import fcntl
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        stream.close()
        raise RuntimeError("a trainer already owns this output directory") from None
    return stream


def verify_export(path, dataset, validation, tool, count=384):
    """C++ FP32 agreement stratified by matchup, action and game phase."""
    selected = {}
    starts = np.array([s["start"] for s in dataset.sequences])
    for start, meta in dataset.metadata_blocks():
        seqs = np.searchsorted(starts, np.arange(start, start + len(meta)), side="right") - 1
        for sequence_index in np.unique(seqs):
            sequence = dataset.sequences[int(sequence_index)]
            if sequence["split"] != "validation":
                continue
            offsets = np.flatnonzero(seqs == sequence_index)
            keys = meta["labels"][offsets].astype(np.int64) * 4 + np.minimum(meta["frames"][offsets] // (24 * 60 * 5), 3)
            _, firsts = np.unique(keys, return_index=True)
            for first in firsts:
                offset = int(offsets[first])
                key = (sequence["matchup"], int(keys[first]))
                selected.setdefault(key, start + offset)
    chosen = sorted(set(selected.values()))
    if len(chosen) < count:
        chosen = sorted(set(chosen + validation[np.linspace(0, len(validation) - 1, count, dtype=int)].tolist()))
    rows = dataset.read_indices(np.array(chosen, dtype=np.int64))
    features, masks, *_ = tensor_batch(rows, len(load_schema()["actions"]))
    model = load_model(path)
    with torch.no_grad():
        expected = model(features)
        probabilities, actions = expected.masked_fill(~masks, -torch.inf).softmax(1).max(1)
    lines = [str(int(mask)) + " " + " ".join(format(float(v), ".9g") for v in row)
             for mask, row in zip(rows["masks"], rows["features"])]
    run = subprocess.run([str(tool), "predict", str(path)], input="\n".join(lines) + "\n",
                         text=True, capture_output=True, check=True, timeout=120)
    actual = [json.loads(line) for line in run.stdout.splitlines() if line.strip()]
    observed = np.array([r["logits"] for r in actual])
    np.testing.assert_allclose(observed, expected.numpy(), rtol=2e-5, atol=1e-4)
    np.testing.assert_allclose([r["probability"] for r in actual], probabilities.numpy(), rtol=2e-5, atol=1e-5)
    np.testing.assert_array_equal([r["action"] for r in actual], actions.numpy())
    return dict(passed=True, samples=len(chosen), strata=len(selected), split="validation",
                max_logit_error=float(np.max(np.abs(observed - expected.numpy()))),
                model_sha256=sha256(path), tool_sha256=sha256(tool), final_test_evaluated=False)


def train(raw_config, resume=False, stop_after_steps=None):
    """stop_after_steps is for interruption tests; the schedule/config stay fixed."""
    config = normalize_config(raw_config)
    output = Path(config["output"])
    output.mkdir(parents=True, exist_ok=resume)
    lock = acquire_lock(output)
    status = Status(output, config["status_seconds"])
    dataset = None
    try:
        device = config["device"]
        if device == "cuda" and not torch.cuda.is_available():
            raise RuntimeError("CUDA required; no silent CPU fallback")
        if config["precision"] == "bf16" and not torch.cuda.is_bf16_supported():
            raise RuntimeError("requested BF16 is unavailable")
        random.seed(config["seed"]); np.random.seed(config["seed"]); torch.manual_seed(config["seed"])
        torch.set_num_threads(config["cpu_threads"])
        torch.use_deterministic_algorithms(True)
        torch.backends.cuda.matmul.allow_tf32 = False
        torch.backends.cudnn.allow_tf32 = False
        status.update(force=True, phase="verifying_shards", device=device, precision=config["precision"])
        dataset = TensorShards(config["dataset"], progress=lambda n, total: status.update(
            verified_rows=n, dataset_rows=total))
        if dataset.manifest["source"] != "human_replay" and not config["allow_synthetic"]:
            raise ValueError("synthetic data requires explicit opt-in")
        if dataset.manifest["source"] == "human_replay" and not config["parity_tool"]:
            raise ValueError("real training requires a compiled export parity tool")
        actions = len(load_schema()["actions"])
        train_rows = sum(s["count"] for s in dataset.sequences if s["split"] == "train")
        validation = split_indices(dataset, "validation")
        steps_per_epoch = config["steps_per_epoch"] or math.ceil(train_rows / config["batch_size"])
        total_steps = steps_per_epoch * config["epochs"]
        identity = dict(config=config, dataset_sha256=dataset.identity, source_hashes=source_identity(),
                        schema=load_schema(), torch=str(torch.__version__), numpy=np.__version__,
                        steps_per_epoch=steps_per_epoch, train_rows=train_rows, validation_rows=len(validation),
                        epoch_definition="sample-equivalent balanced draws with replacement",
                        device_name=torch.cuda.get_device_name() if device == "cuda" else "CPU",
                        parity_tool_sha256=sha256(config["parity_tool"]) if config["parity_tool"] else None)
        model = MacroModel(tuple(config["hidden"])).to(device)
        optimizer = torch.optim.AdamW(model.parameters(), lr=config["learning_rate"],
                                     weight_decay=config["weight_decay"], fused=device == "cuda")
        sampler = BalancedRows(dataset, config["batch_size"], config["seed"])
        state = dict(epoch=0, epoch_step=0, best=None, stale=0, history=[], epoch_loss=0.0, epoch_weight=0.0)
        if resume:
            # Only read this run's own trusted checkpoint, never downloaded pickle files.
            saved = torch.load(output / "latest.pt", map_location="cpu", weights_only=False)
            if saved["format"] != "protodd-shard-training-v1" or saved["identity"] != identity:
                raise ValueError("checkpoint configuration, data or source mismatch")
            model.load_state_dict(saved["model"])
            optimizer.load_state_dict(saved["optimizer"])
            sampler.load_state_dict(saved["sampler"])
            state = saved["state"]
            restore_rng(saved["rng"])
            baseline = json.loads((output / "frequency_baseline.json").read_text())
        else:
            atomic_json(output / "config.json", identity)
            freeze_sources(output, identity)
            status.update(force=True, phase="frequency_baseline", parameters=sum(p.numel() for p in model.parameters()))
            baseline = evaluate(FrequencyBaseline(training_prior(dataset)).to(device), dataset,
                                validation, config, status, "frequency_baseline")
            atomic_json(output / "frequency_baseline.json", baseline)

        def checkpoint(name="latest.pt"):
            atomic_checkpoint(output / name, dict(format="protodd-shard-training-v1", identity=identity,
                model=model.state_dict(), optimizer=optimizer.state_dict(), sampler=sampler.state_dict(),
                state=state, rng=rng_state(), schedule=dict(kind="warmup_cosine_10pct", next_step=sampler.consumed,
                                                          total_steps=total_steps)))

        if not resume:
            checkpoint()
        cache = None
        if device == "cuda" and config["precision"] == "bf16" and config["data_cache"] != "stream":
            required = CudaTrainingCache.required_bytes(dataset)
            free, _ = torch.cuda.mem_get_info()
            # Leave two GiB for activations, validation, CUDA and desktop changes.
            fits = required + 2 * 2**30 < free
            if not fits and config["data_cache"] == "cuda":
                raise RuntimeError(f"GPU cache needs {required / 2**30:.2f} GiB plus 2 GiB headroom; only {free / 2**30:.2f} GiB free")
            if fits:
                status.update(force=True, phase="loading_gpu_cache", cache_bytes=required,
                              cache_rows_loaded=0, cache_rows_total=train_rows)
                cache = CudaTrainingCache(dataset, progress=lambda n, total: status.update(
                    cache_rows_loaded=n, cache_rows_total=total))
        status.state["data_cache"] = "cuda_bf16" if cache is not None else "stream"
        status.update(force=True, phase="training", step=sampler.consumed, total_steps=total_steps,
                      epoch=state["epoch"] + 1, max_epochs=config["epochs"], steps_per_epoch=steps_per_epoch,
                      train_rows=train_rows, validation_rows_total=len(validation))
        fit_seconds = 0.0
        measured_steps = 0
        loader_seconds = 0.0
        if device == "cuda":
            torch.cuda.reset_peak_memory_stats()
        while state["epoch"] < config["epochs"] and state["stale"] < config["patience"]:
            model.train()
            stop = (state["epoch"] + 1) * steps_per_epoch
            def read(index):
                return tensor_batch(dataset.read_indices(sampler.indices_at(index)), actions, device == "cuda")
            batches = ((index, cache.batch(sampler.indices_at(index))) for index in range(sampler.consumed, stop)) if cache is not None else prefetched(range(sampler.consumed, stop), read, config["prefetch"])
            previous = time.perf_counter()
            try:
                for index, batch in batches:
                    ready = time.perf_counter()
                    loader_seconds += ready - previous
                    features, masks, labels, weights, _ = (t.to(device, non_blocking=True) for t in batch)
                    lr = learning_rate(config, index, total_steps)
                    for group in optimizer.param_groups:
                        group["lr"] = lr
                    optimizer.zero_grad(set_to_none=True)
                    with torch.autocast(device_type=device, dtype=torch.bfloat16,
                                        enabled=config["precision"] == "bf16"):
                        raw = model(features)
                    logits = raw.float().masked_fill(~masks, -torch.inf)
                    weight_sum = weights.sum()
                    loss_sum = (F.cross_entropy(logits, labels, reduction="none") * weights).sum()
                    loss = loss_sum / weight_sum
                    if not torch.isfinite(loss):
                        raise ValueError("nonfinite training loss")
                    loss.backward()
                    torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0, error_if_nonfinite=True)
                    optimizer.step()
                    # Two scalars synchronize each committed update; no speculative
                    # optimizer/RNG state is ever marked consumed in a checkpoint.
                    state["epoch_loss"] += loss_sum.item()
                    state["epoch_weight"] += weight_sum.item()
                    sampler.acknowledge(index)
                    state["epoch_step"] += 1
                    measured_steps += 1
                    fit_seconds += time.perf_counter() - previous
                    if sampler.consumed % config["checkpoint_steps"] == 0:
                        checkpoint()
                        status.state["checkpoint_step"] = sampler.consumed
                    gpu = dict(gpu_allocated_mib=torch.cuda.memory_allocated() / 2**20,
                               gpu_reserved_mib=torch.cuda.memory_reserved() / 2**20,
                               gpu_peak_allocated_mib=torch.cuda.max_memory_allocated() / 2**20) if device == "cuda" else {}
                    status.update(phase="training", training_started=True, step=sampler.consumed,
                        epoch=state["epoch"] + 1, epoch_step=state["epoch_step"], learning_rate=lr,
                        train_loss=state["epoch_loss"] / state["epoch_weight"],
                        examples_per_second=measured_steps * config["batch_size"] / fit_seconds,
                        mean_step_seconds=fit_seconds / measured_steps,
                        loader_wait_fraction=loader_seconds / fit_seconds,
                        estimated_training_seconds_remaining=(total_steps - sampler.consumed) * fit_seconds / measured_steps,
                        **gpu)
                    previous = time.perf_counter()
                    if stop_after_steps is not None and sampler.consumed >= stop_after_steps:
                        checkpoint()
                        status.update(force=True, phase="interrupted_for_resume", checkpoint_step=sampler.consumed)
                        return dict(step=sampler.consumed, interrupted=True)
            finally:
                batches.close()
            status.update(force=True, phase="validation", validation_rows_processed=0)
            report = evaluate(model, dataset, validation, config, status)
            score = report["matchup_balanced_game"]["cross_entropy"]
            improved = state["best"] is None or score < state["best"]
            state["history"].append(dict(epoch=state["epoch"] + 1, step=sampler.consumed,
                train_loss=state["epoch_loss"] / state["epoch_weight"], validation=report))
            state["epoch"] += 1
            state["epoch_step"] = 0
            state["epoch_loss"], state["epoch_weight"] = 0.0, 0.0
            state["best"] = score if improved else state["best"]
            state["stale"] = 0 if improved else state["stale"] + 1
            if improved:
                checkpoint("best.pt")
                export_model(model, output / "LearnedMacro.bin.partial")
                os.replace(output / "LearnedMacro.bin.partial", output / "LearnedMacro.bin")
            atomic_json(output / "history.json", state["history"])
            checkpoint()
            status.update(force=True, phase="epoch_complete", completed_epochs=state["epoch"],
                          best_validation_cross_entropy=state["best"], validation_cross_entropy=score,
                          baseline_cross_entropy=baseline["matchup_balanced_game"]["cross_entropy"],
                          checkpoint_step=sampler.consumed)

        # Regenerate from the best checkpoint even when resuming after interruption
        # during export; then validate the exact FP32 artifact that will be shipped.
        best = torch.load(output / "best.pt", map_location="cpu", weights_only=False)
        model.load_state_dict(best["model"])
        export_model(model, output / "LearnedMacro.bin.partial")
        os.replace(output / "LearnedMacro.bin.partial", output / "LearnedMacro.bin")
        status.update(force=True, phase="export_validation", validation_rows_processed=0)
        restored = load_model(output / "LearnedMacro.bin").to(device)
        final_report = evaluate(restored, dataset, validation, config, status, "export_validation")
        parity = verify_export(output / "LearnedMacro.bin", dataset, validation, config["parity_tool"]) if config["parity_tool"] else None
        if parity:
            atomic_json(output / "export_parity.json", parity)
        result = dict(config=identity, model_sha256=sha256(output / "LearnedMacro.bin"),
                      validation=final_report, frequency_baseline=baseline, export_parity=parity,
                      steps_completed=sampler.consumed, epochs_completed=state["epoch"],
                      best_epoch=best["state"]["epoch"], deployment="shadow-only",
                      strength_validated=False, final_test_evaluated=False,
                      synthetic_only=dataset.manifest["source"] != "human_replay")
        atomic_json(output / "manifest.json", result)
        status.update(force=True, phase="complete", completed_epochs=state["epoch"],
                      best_epoch=best["state"]["epoch"], model_sha256=result["model_sha256"],
                      estimated_training_seconds_remaining=0)
        return result
    except BaseException as error:
        status.update(force=True, phase="failed", error=f"{type(error).__name__}: {error}")
        raise
    finally:
        if dataset is not None:
            dataset.close()
        lock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    train(json.loads(args.config.read_text(encoding="utf-8")), resume=args.resume)


if __name__ == "__main__":
    main()
