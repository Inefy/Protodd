"""Bounded end-to-end capacity diagnostic on fixed omitted-train windows.

Checks whether the complete existing network can learn a tiny balanced set.
Development games are disjoint train games, already used for development.
This is not a candidate fit, fresh validation result, or runtime controller.
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
from .whole_game_multislot_actor_rank_fit import cohorts
from .whole_game_multislot_audit import score_sequence, summarize
from .whole_game_multislot_batch import multislot_minibatch_loss
from .whole_game_multislot_collect import collect_multislot_windows
from .whole_game_multislot_fit import _atomic_save
from .whole_game_multislot_model import MultiSlotWholeGameModel


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def write_json(path, value):
    temporary = path.with_suffix(".tmp.json")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def sample_key(sample):
    return f"{sample[4]['game_id']}:{sample[1]['frame']}:{sample[1]['sequence']}"


def balanced_subset(buckets, per_matchup, seed):
    rng = random.Random(seed)
    result = []
    for matchup in ("PvP", "PvT", "PvZ"):
        keys = sorted(key for key in buckets if key[0] == matchup)
        # Cover both combat kinds and STOP even when the limit is small.
        priority = {name: i for i, name in enumerate(
            ("attack", "attack_move", "stop", "right_click", "build", "train"))}
        keys.sort(key=lambda key: (priority.get(key[2], 99), key))
        queues = {key: rng.sample(buckets[key], len(buckets[key])) for key in keys}
        chosen = []
        while len(chosen) < per_matchup and any(queues.values()):
            for key in keys:
                if queues[key] and len(chosen) < per_matchup:
                    chosen.append(queues[key].pop())
        if len(chosen) != per_matchup:
            raise ValueError("not enough windows for the fixed capacity set")
        result.extend(chosen)
    if len({sample_key(s) for s in result}) != len(result):
        raise ValueError("duplicate fixed windows")
    return result


def evaluate(model, samples, device, cache):
    model.eval()
    losses, rows = [], []
    actor_sets = Counter()
    # Cache entries are reused during gradient updates. no_grad keeps these as
    # ordinary tensors; inference_mode would make cached embedding indices
    # unusable by autograd in the subsequent training phase.
    with torch.no_grad():
        for sample in samples:
            loss, _ = multislot_minibatch_loss(model, [sample], device, cache)
            losses.append(float(loss))
            memory = None
            for row in sample[0]:
                batch, _, _ = cache.get(row, sample[3])
                memory = model({k: v.to(device) for k, v in batch.items()}, memory)["memory"]
            batch, ids, _ = cache.get(sample[1], sample[3])
            prediction = model.forward_slots({k: v.to(device) for k, v in batch.items()}, memory)
            scored = score_sequence(sample[2], prediction["slots"], ids,
                                    sample[3].shape[1], sample[3].shape[0])
            scored.update(game_id=sample[4]["game_id"], matchup=sample[4]["matchup"])
            rows.append(scored)
            for i, label in enumerate(sample[2]["labels"][:model.maximum_slots]):
                if not sample[2]["actor_available"][i]:
                    continue
                gold = set(label["actor_positive"])
                logits = prediction["slots"][i]["actor"][0]
                predicted = {token for token, index in ids.items() if float(logits[index]) >= 0} \
                    if i < scored["predicted_commands"] else set()
                known = gold | set(label["actor_negative"])
                actor_sets["known_actor_true_positives"] += len(predicted & gold)
                actor_sets["known_actor_false_positives"] += len((predicted & known) - gold)
                actor_sets["known_actor_false_negatives"] += len(gold - predicted)
                actor_sets["predictions_without_actor_supervision"] += len(predicted - known)
                actor_sets["fully_matching_sets"] += predicted == gold
                actor_sets["commands"] += 1
    return dict(mean_teacher_loss=sum(losses) / len(losses),
                free_running=summarize(rows),
                diagnostic_actor_sets_at_fixed_logit_zero=dict(actor_sets),
                actor_set_note="Diagnostic only; deployed decoding still chooses one actor.")


def run(args):
    if args.steps < 1 or args.windows_per_matchup < 6 or args.batch_size < 1 or args.max_seconds < 1:
        raise ValueError("invalid experiment limits")
    if not torch.cuda.is_available():
        raise RuntimeError("this bounded experiment requires the local CUDA GPU")
    if args.output.exists() and not args.resume:
        raise FileExistsError(args.output)
    torch.set_num_threads(2)
    torch.use_deterministic_algorithms(True)
    torch.manual_seed(args.seed)
    torch.cuda.manual_seed_all(args.seed)
    rng = random.Random(args.seed)
    full_spec = json.loads(args.full_fit_run.read_text(encoding="utf-8"))
    train_ids, dev_ids = cohorts(args.release, full_spec, train_games_per_matchup=1)
    dev_ids = {key: [value] for key, value in dev_ids.items()}
    saved = torch.load(args.checkpoint, map_location="cpu", weights_only=True)
    # Checkpoints use the identity FILE hash; shard receipts separately verify
    # the canonical identity hash inside selected_shards.
    identity_sha = sha(args.release / "identity.json")
    if saved["source_identity_sha256"] != identity_sha:
        raise ValueError("checkpoint and verified release differ")
    spec = dict(schema="protodd-capacity-diagnostic-v1", checkpoint_sha256=sha(args.checkpoint),
                full_fit_run_sha256=sha(args.full_fit_run), release_identity_sha256=identity_sha,
                source_sha256={p.name: sha(p) for p in sorted(Path(__file__).parent.glob("*.py"))},
                train_games=train_ids, dev_games=dev_ids, steps=args.steps,
                windows_per_matchup=args.windows_per_matchup, batch_size=args.batch_size,
                seed=args.seed, learning_rate=args.learning_rate, history=8,
                per_game_category_limit=64, category_limit=64,
                max_seconds=args.max_seconds, device="cuda", amp="bfloat16",
                memory_contract="reconstruct eight prior cadence rows from zero for every train/eval window",
                purpose="tiny-set capacity and exposure sanity, no promotion", promotion_eligible=False,
                capacity_gate=dict(teacher_loss_ratio_max=0.3, training_signature_fraction_min=0.5,
                                   both_combat_kind_matches_required=True),
                runtime_memory_parity=False, fresh_validation=False)
    if args.resume:
        if json.loads((args.output / "run.json").read_text()) != spec:
            raise ValueError("resume source, data or experiment specification changed")
    else:
        args.output.mkdir(parents=True)
        write_json(args.output / "run.json", spec)
    def status(stage, **extra):
        write_json(args.output / "status.json", dict(stage=stage, pid=os.getpid(),
                   updated_unix=time.time(), promotion_eligible=False, **extra))
    status("collecting")
    train_buckets, train_info = collect_multislot_windows(args.release, "train", train_ids,
        history=8, per_game_category_limit=64, category_limit=64, seed=args.seed)
    dev_buckets, dev_info = collect_multislot_windows(args.release, "train", dev_ids,
        history=8, per_game_category_limit=64, category_limit=64, seed=args.seed)
    train = balanced_subset(train_buckets, args.windows_per_matchup, args.seed)
    dev = balanced_subset(dev_buckets, args.windows_per_matchup, args.seed)
    selection = dict(train=[sample_key(s) for s in train], dev=[sample_key(s) for s in dev],
                     collection=dict(train=train_info, dev=dev_info))
    if args.resume and json.loads((args.output / "selection.json").read_text()) != selection:
        raise ValueError("fixed windows changed on resume")
    write_json(args.output / "selection.json", selection)
    del train_buckets, dev_buckets
    model = MultiSlotWholeGameModel(width=full_spec["width"],
        mixture_components=full_spec["mixture_components"], maximum_slots=full_spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to("cuda")
    initial_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.learning_rate)
    cache = ObservationCache(256 * 1024 * 1024)
    presented, start_step, prior_seconds = Counter(), 0, 0.0
    if args.resume:
        checkpoint = torch.load(args.output / "resume.pt", map_location="cpu", weights_only=True)
        model.load_state_dict(checkpoint["model"])
        optimizer.load_state_dict(checkpoint["optimizer"])
        rng.setstate(checkpoint["python_rng"])
        torch.set_rng_state(checkpoint["torch_rng"])
        torch.cuda.set_rng_state_all(checkpoint["cuda_rng"])
        presented.update(checkpoint["presented"])
        start_step, prior_seconds = checkpoint["step"], checkpoint["training_seconds"]
        before = json.loads((args.output / "before.json").read_text())
    else:
        status("baseline_evaluation")
        before = dict(train=evaluate(model, train, "cuda", cache), dev=evaluate(model, dev, "cuda", cache))
        write_json(args.output / "before.json", before)
    shapes = defaultdict(list)
    for sample in train:
        shapes[tuple(sample[3].shape)].append(sample)
    keys = list(shapes)
    began, losses, step = time.monotonic(), [], start_step
    def checkpoint():
        _atomic_save(args.output / "resume.pt", dict(model=model.state_dict(),
            optimizer=optimizer.state_dict(), python_rng=rng.getstate(),
            torch_rng=torch.get_rng_state(), cuda_rng=torch.cuda.get_rng_state_all(),
            step=step, presented=dict(presented), training_seconds=prior_seconds + time.monotonic() - began))
    status("training", step=step, total_steps=args.steps)
    model.train()
    for step in range(start_step + 1, args.steps + 1):
        bucket = shapes[rng.choices(keys, weights=[len(shapes[k]) for k in keys])[0]]
        samples = rng.sample(bucket, min(args.batch_size, len(bucket)))
        optimizer.zero_grad(set_to_none=True)
        with torch.autocast(device_type="cuda", dtype=torch.bfloat16):
            loss, _ = multislot_minibatch_loss(model, samples, "cuda", cache, report=False)
        if not torch.isfinite(loss):
            raise ValueError("nonfinite capacity loss")
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        optimizer.step()
        presented.update(sample_key(s) for s in samples)
        losses.append(float(loss.detach()))
        elapsed = prior_seconds + time.monotonic() - began
        if step % 50 == 0 or step == args.steps or elapsed >= args.max_seconds:
            checkpoint()
            row = dict(step=step, total_steps=args.steps, loss=sum(losses) / len(losses),
                       training_seconds=round(elapsed, 2), unique_windows=len(presented),
                       window_presentations=sum(presented.values()))
            with (args.output / "progress.jsonl").open("a", encoding="utf-8") as stream:
                stream.write(json.dumps(row) + "\n")
            print(json.dumps(row), flush=True)
            status("training", **row)
            losses.clear()
        if elapsed >= args.max_seconds:
            break
    status("final_evaluation", step=step)
    after = dict(train=evaluate(model, train, "cuda", cache), dev=evaluate(model, dev, "cuda", cache))
    train_score = after["train"]["free_running"]
    ratio = after["train"]["mean_teacher_loss"] / before["train"]["mean_teacher_loss"]
    signatures = train_score["full_signature_correct"] / max(1, train_score["slot_commands"])
    combat = all(train_score["kind_by_name"].get(kind, {}).get("correct", 0) > 0
                 for kind in ("attack", "attack_move"))
    changed = [name for name, value in model.state_dict().items()
               if not torch.equal(value.detach().cpu(), initial_state[name])]
    report = dict(promotion_eligible=False, strength_validated=False, steps=step,
        stop_reason="steps" if step == args.steps else "training_time_budget",
        before=before, after=after, unique_gradient_windows=len(presented),
        window_presentations=sum(presented.values()), presentation_counts=dict(presented),
        changed_tensors=changed, teacher_loss_ratio=ratio, train_signature_fraction=signatures,
        capacity_pass=ratio <= 0.3 and signatures >= 0.5 and combat,
        interpretation="Capacity success permits further bounded development only; no game strength or group execution claim.")
    write_json(args.output / "report.json", report)
    status("complete", step=step, capacity_pass=report["capacity_pass"])
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "release", "full_fit_run", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--steps", type=int, default=600)
    parser.add_argument("--windows-per-matchup", type=int, default=16)
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--seed", type=int, default=20260924)
    parser.add_argument("--max-seconds", type=int, default=1200)
    parser.add_argument("--resume", action="store_true")
    args = parser.parse_args()
    try:
        result = run(args)
        print(json.dumps({k: result[k] for k in ("steps", "capacity_pass", "teacher_loss_ratio", "train_signature_fraction")}), flush=True)
    except Exception as error:
        status_path = args.output / "status.json"
        if status_path.exists():
            state = json.loads(status_path.read_text())
            if state.get("pid") == os.getpid():
                write_json(args.output / "failure.json", dict(error=repr(error), unix=time.time()))
                state.update(stage="failed", error=repr(error), updated_unix=time.time())
                write_json(status_path, state)
        raise
