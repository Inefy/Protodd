"""Separate teacher-context, actor-choice and position-mixture failures.

Only opens the fixed TRAIN windows from the completed local capacity diagnostic.
Oracle components and replay conditioning are diagnostics, never policy scores.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import statistics

import torch

from .whole_game_capacity_fit import balanced_subset, sample_key
from .whole_game_encoding_cache import ObservationCache
from .whole_game_multislot_batch import multislot_minibatch_loss
from .whole_game_multislot_collect import collect_multislot_windows
from .whole_game_multislot_model import MultiSlotWholeGameModel


def audit(run, release, full_fit_run, output):
    run, release, full_fit_run, output = map(Path, (run, release, full_fit_run, output))
    if output.exists():
        raise FileExistsError(output)
    spec = json.loads((run / "run.json").read_text())
    selection = json.loads((run / "selection.json").read_text())
    architecture = json.loads(full_fit_run.read_text())
    buckets, _ = collect_multislot_windows(release, "train", spec["train_games"],
        history=spec["history"], per_game_category_limit=spec["per_game_category_limit"],
        category_limit=spec["category_limit"], seed=spec["seed"])
    samples = balanced_subset(buckets, spec["windows_per_matchup"], spec["seed"])
    if [sample_key(s) for s in samples] != selection["train"]:
        raise ValueError("fixed training windows changed")
    model = MultiSlotWholeGameModel(width=architecture["width"],
        mixture_components=architecture["mixture_components"], maximum_slots=architecture["maximum_slots"])
    model.load_state_dict(torch.load(run / "resume.pt", map_location="cpu", weights_only=True)["model"])
    model.cuda().eval()
    torch.set_num_threads(2)
    cache = ObservationCache()
    counts, rows = Counter(), []
    with torch.no_grad():
        for sample in samples:
            original = model.forward_slots
            captured = {}
            def capture(batch, memory=None, **kwargs):
                result = original(batch, memory, **kwargs)
                captured.update(batch=batch, memory=memory, kwargs=kwargs, result=result)
                return result
            model.forward_slots = capture
            try:
                multislot_minibatch_loss(model, [sample], "cuda", cache)
            finally:
                model.forward_slots = original
            free = original(captured["batch"], captured["memory"])["slots"]
            tokens = captured["kwargs"]["teacher_tokens"]
            for index, label in enumerate(sample[2]["labels"][:model.maximum_slots]):
                teacher = captured["result"]["slots"][index]
                counts["commands"] += 1
                counts["teacher_context_actor_argmax_is_teacher_anchor"] += int(teacher["actor"].argmax(-1)) == int(tokens[index]["actor"])
                counts["teacher_context_kind_argmax_correct"] += int(teacher["kind"].argmax(-1)) == int(tokens[index]["kind"])
                if not label["loss_masks"]["target_position"]:
                    continue
                params = teacher["position"][0]
                factor = torch.tensor([sample[3].shape[1] * 32, sample[3].shape[0] * 32], device="cuda")
                truth = tokens[index]["position"][0] * factor
                errors = ((params[:, 1:3].sigmoid() * factor - truth).square().sum(-1)).sqrt()
                component = int(params[:, 0].argmax())
                free_error = float(((free[index]["chosen_position"][0] * factor - truth).square().sum()).sqrt())
                rows.append(dict(window=sample_key(sample), slot=index, kind=label["actions"]["kind"],
                    teacher_highest_weight_error_px=float(errors[component]),
                    oracle_nearest_component_error_px=float(errors.min()),
                    free_output_position_error_px=free_error,
                    highest_weight_is_nearest=component == int(errors.argmin()),
                    mixture_weights=params[:, 0].softmax(-1).cpu().tolist()))
    summary = dict(counts)
    for metric in ("teacher_highest_weight_error_px", "oracle_nearest_component_error_px", "free_output_position_error_px"):
        values = [r[metric] for r in rows]
        summary[metric] = dict(samples=len(values), within64=sum(v <= 64 for v in values), median=statistics.median(values))
    report = dict(schema="protodd-conditioning-audit-v1", promotion_eligible=False,
        source_sha256={p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(Path(__file__).parent.glob("*.py"))},
        checkpoint_sha256=hashlib.sha256((run / "resume.pt").read_bytes()).hexdigest(),
        selection_sha256=hashlib.sha256((run / "selection.json").read_bytes()).hexdigest(),
        summary=summary, rows=rows,
        note="Teacher context and best-of-mixture require replay labels. Not causal policy quality; train-only diagnosis.")
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
    return summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("run", "release", "full_fit_run", "output"):
        parser.add_argument(name, type=Path)
    print(json.dumps(audit(**vars(parser.parse_args())), indent=2))
