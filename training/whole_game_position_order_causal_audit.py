"""Paired causal audit of a fixed current-order position arbiter.

The decoder runs freely on the 24-game disjoint validation cohort. The arbiter
uses only the predicted actor and its current cadence observation. This is an
offline diagnostic and does not grant live or tournament control.
"""
from __future__ import annotations

from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

import torch

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_features import encode_observation, static_grid
from .whole_game_fit import MATCHUPS, choose_games, validate_release_pair
from .whole_game_model import TARGET_MODES
from .whole_game_multislot_audit import score_sequence, summarize
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_release import key
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def apply_order_rule(slots, sequence, ids, width, height):
    reverse_ids = {index: token for token, index in ids.items()}
    own = {entity["id"]: entity for entity in sequence["observation"]["entities"]
           if entity["relation"] == 0}
    patched = [dict(slot) for slot in slots]
    counts = Counter()
    for index, slot in enumerate(slots):
        if (int(slot["chosen_stop"][0]) == 1 or
                TARGET_MODES[int(slot["chosen_target_mode"][0])] != "position"):
            continue
        counts["position_slots"] += 1
        actor_id = reverse_ids.get(int(slot["chosen_actor"][0]))
        actor = own.get(actor_id)
        if actor is None:
            raise ValueError("predicted actor is not owned")
        order_xy = actor["own_state"]["order_position"]
        if min(order_xy) < 0:
            continue
        counts["valid_order"] += 1
        original = slot["chosen_position"][0]
        predicted_px = original * torch.tensor((width * 32, height * 32),
                                               device=original.device)
        order_px = torch.tensor(order_xy, dtype=torch.float32,
                                device=original.device)
        if float(torch.linalg.vector_norm(order_px - predicted_px)) > 256:
            continue
        replacement = slot["chosen_position"].clone()
        replacement[0] = order_px / torch.tensor((width * 32, height * 32),
                                                  device=original.device)
        patched[index]["chosen_position"] = replacement
        counts["used_order"] += 1
    return patched, counts


def audit(checkpoint, train_release, validation_release, development_report,
          source_audit_report, output, *, device="cuda", games_per_matchup=8,
          seed=42, reset_interval=8):
    (checkpoint, train_release, validation_release, development_report,
     source_audit_report, output) = map(Path, (
        checkpoint, train_release, validation_release, development_report,
        source_audit_report, output))
    if output.exists() or games_per_matchup < 1 or reset_interval < 1:
        raise ValueError("existing output or invalid audit limits")
    train_sha, validation_sha = validate_release_pair(train_release, validation_release)
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if saved["source_identity_sha256"] != train_sha:
        raise ValueError("source checkpoint and release disagree")
    spec = json.loads((checkpoint.parent / "run.json").read_text(encoding="utf8"))
    model = MultiSlotWholeGameModel(
        width=spec["width"], mixture_components=spec["mixture_components"],
        maximum_slots=spec["maximum_slots"])
    model.load_state_dict(saved["state_dict"], strict=True)
    model.to(device).eval()
    torch.set_num_threads(2)
    identity = json.loads((validation_release / "identity.json").read_text(encoding="utf8"))
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if (record["split"] == "validation" and record["matchup"] in MATCHUPS and
                (validation_release / "games" / key(record) / "receipt.json").exists()):
            eligible[record["matchup"]].append(record)
    chosen, _ = choose_games(eligible, "validation", games_per_matchup, seed)
    games, changes = Counter(), Counter()
    baseline_rows, candidate_rows = [], []
    with torch.inference_mode():
        for record, directory, _ in selected_shards(
                validation_release, "validation", game_ids=chosen):
            games[record["matchup"]] += 1
            terrain = static_grid(load_terrain(directory))
            width, height = terrain.shape[1], terrain.shape[0]
            memory = None
            for index, sequence in enumerate(cadence_sequences(trajectory_shard(directory))):
                if index % reset_interval == 0:
                    memory = None
                if sequence["observation"]["reason"] != "cadence":
                    raise ValueError("noncausal order observation")
                batch, ids, _ = encode_observation(sequence["observation"], terrain)
                prediction = model.forward_slots(
                    {name: value.to(device) for name, value in batch.items()}, memory)
                memory = prediction["backbone"]["memory"]
                original = prediction["slots"]
                changed, counts = apply_order_rule(
                    original, sequence, ids, width, height)
                changes.update(counts)
                for rows, slots in ((baseline_rows, original),
                                    (candidate_rows, changed)):
                    scored = score_sequence(sequence, slots, ids, width, height)
                    scored.update(game_id=record["game_id"], matchup=record["matchup"])
                    rows.append(scored)
    if any(games[matchup] != games_per_matchup for matchup in MATCHUPS):
        raise ValueError("causal validation cohort incomplete")
    baseline, candidate = summarize(baseline_rows), summarize(candidate_rows)
    reference = json.loads(source_audit_report.read_text(encoding="utf8"))
    expected = reference["overall"]
    for name in ("windows", "slot_commands", "full_signature_correct",
                 "position_within_64px", "kind_correct", "actor_correct"):
        if baseline[name] != expected[name]:
            raise ValueError(f"paired baseline differs from source audit: {name}")
    report = dict(
        schema="protodd-whole-game-position-order-causal-audit-v1",
        promotion_eligible=False, live_control_allowed=False,
        order_agreement_radius_px=256, memory_reset_interval=reset_interval,
        source_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
        source_sha256=hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
        development_report_sha256=hashlib.sha256(development_report.read_bytes()).hexdigest(),
        source_audit_report_sha256=hashlib.sha256(source_audit_report.read_bytes()).hexdigest(),
        train_identity_sha256=train_sha, validation_identity_sha256=validation_sha,
        games=dict(games), changed=dict(changes), baseline=baseline,
        candidate=candidate,
        by_matchup={matchup: dict(baseline=summarize([row for row in baseline_rows
                                                     if row["matchup"] == matchup]),
                                candidate=summarize([row for row in candidate_rows
                                                      if row["matchup"] == matchup]))
                    for matchup in MATCHUPS})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("checkpoint", "train_release", "validation_release",
                 "development_report", "source_audit_report", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--device", choices=("cpu", "cuda"), default="cuda")
    args = parser.parse_args()
    result = audit(args.checkpoint, args.train_release, args.validation_release,
                   args.development_report, args.source_audit_report, args.output,
                   device=args.device)
    print(json.dumps(dict(changed=result["changed"],
                          baseline={key: result["baseline"][key] for key in
                                    ("full_signature_correct", "position_within_64px")},
                          candidate={key: result["candidate"][key] for key in
                                     ("full_signature_correct", "position_within_64px")}),
                     indent=2))
