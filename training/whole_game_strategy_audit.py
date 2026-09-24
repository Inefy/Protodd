"""Measure fit exposure and group-command coverage without fitting a model.

Uses three previously inspected development games from the TRAIN split, omitted
from the full fit. This is a contract diagnostic, never a held-out strength test.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import hashlib
import json
from pathlib import Path

from .whole_game_cadence_sequences import cadence_sequences
from .whole_game_shards import selected_shards, trajectory_shard


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def audit(release, fit_run, fit_log, output):
    release, fit_run, fit_log, output = map(Path, (release, fit_run, fit_log, output))
    if output.exists():
        raise ValueError("use a new report path; evidence is immutable")
    spec = json.loads(fit_run.read_text(encoding="utf-8"))
    identity = json.loads((release / "identity.json").read_text(encoding="utf-8"))
    trained = {gid for group in spec["groups"] for ids in group.values() for gid in ids}
    eligible = defaultdict(list)
    for record in identity["selected"]:
        if record["split"] == "train" and record["game_id"] not in trained:
            eligible[record["matchup"]].append(record["game_id"])
    if set(eligible) != {"PvP", "PvT", "PvZ"}:
        raise ValueError("three omitted-train matchup cohorts required")
    chosen = {matchup: sorted(ids)[0] for matchup, ids in eligible.items()}
    raw = fit_log.read_bytes()
    log_text = raw.decode("utf-16" if raw[:2] in (b"\xff\xfe", b"\xfe\xff") else "utf-8-sig")
    groups = []
    for line in log_text.splitlines():
        try:
            row = json.loads(line)
        except ValueError:
            continue
        if row.get("stage") == "fit":
            groups.append(row)
    if [row["group"] for row in groups] != list(range(1, len(spec["groups"]) + 1)):
        raise ValueError("fit log is incomplete, repeated or out of order")
    presented = sum((Counter(row["examples"]) for row in groups), Counter())
    counts, sizes = Counter(), Counter()
    kinds, receipts = defaultdict(Counter), {}
    for record, directory, _ in selected_shards(
            release, "train", require_complete=True, game_ids=set(chosen.values())):
        receipts[record["game_id"]] = digest(directory / "receipt.json")
        for sequence in cadence_sequences(trajectory_shard(directory)):
            counts["windows"] += 1
            counts["all_commands"] += len(sequence["labels"])
            for label in sequence["labels"][:spec["maximum_slots"]]:
                size = len(set(label["actor_positive"]))
                sizes[size] += 1
                for bucket in (counts, kinds[label["actions"]["kind"]]):
                    bucket["slot_commands"] += 1
                    bucket["commands_with_multiple_actors"] += size > 1
                    bucket["actor_memberships"] += size
                    bucket["one_actor_membership_ceiling"] += size > 0
    if set(receipts) != set(chosen.values()):
        raise ValueError("diagnostic cohort is incomplete")
    root = Path(__file__).resolve().parents[1]
    sources = ["training/whole_game_strategy_audit.py", "training/whole_game_shards.py",
               "training/whole_game_sequences.py", "training/whole_game_cadence_sequences.py",
               "training/whole_game_multislot_collect.py", "training/whole_game_multislot_fit.py",
               "training/whole_game_model.py", "training/whole_game_multislot_model.py",
               "training/whole_game_multislot_audit.py", "src/bwapi/WholeGameRuntime.cpp"]
    report = dict(
        schema="protodd-whole-game-strategy-audit-v1", strength_validated=False,
        inputs={str(path): digest(path) for path in (release / "identity.json", fit_run, fit_log)},
        source_sha256={name: digest(root / name) for name in sources},
        fit_exposure=dict(games=len(trained), groups=len(groups),
                          updates=len(groups) * spec["steps_per_group"],
                          retained_windows=sum(row["retained"] for row in groups),
                          window_presentations=sum(presented.values()),
                          presentations_by_sampling_mode=dict(presented),
                          unique_gradient_windows="not logged; retained windows are only an upper bound",
                          epochs=spec["epochs"], history=spec["history"],
                          per_game_category_limit=spec["per_game_category_limit"]),
        diagnostic=dict(split="train", game_ids=chosen, receipt_sha256=receipts,
                        previously_used_for_development=True, counts=dict(counts),
                        actor_set_size_histogram=dict(sorted(sizes.items())),
                        by_kind={kind: dict(value) for kind, value in sorted(kinds.items())}),
        interpretation=[
            "One-actor membership ceiling assumes a correct selected unit in every packet; it is not accuracy.",
            "Memberships count unit-command pairs, not unique units; this cohort is not a corpus estimate.",
            "Existing actor scoring accepts any selected member; it does not measure complete group execution.",
            "Replay packet accuracy and retained-window counts do not establish game strength."])
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("x", encoding="utf-8") as stream:
        json.dump(report, stream, indent=2)
        stream.write("\n")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "fit_run", "fit_log", "output"):
        parser.add_argument(name, type=Path)
    report = audit(**vars(parser.parse_args()))
    print(json.dumps(dict(fit_exposure=report["fit_exposure"],
                          command_contract=report["diagnostic"]["counts"]), indent=2))
