"""Compare action audits on identical disjoint replay examples and a prior baseline."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path


def compare(named_reports):
    if len(named_reports) < 2:
        raise ValueError("at least two candidate audits required")
    reports = {name: json.loads(Path(path).read_text(encoding="utf8"))
               for name, path in named_reports.items()}
    reference = next(iter(reports.values()))
    if reference.get("sample_mode") not in ("balanced", "natural"):
        raise ValueError("unknown action audit sample mode")
    def keys(report):
        return [(row["game_id"], row["frame"], row["true_kind"], row["true_target_mode"])
                for row in report["rows"]]
    expected = keys(reference)
    if not expected:
        raise ValueError("empty action audit")
    for name, report in reports.items():
        if (report.get("validation_identity_sha256") != reference["validation_identity_sha256"] or
                report.get("sample_mode") != reference["sample_mode"] or
                keys(report) != expected):
            raise ValueError(f"{name} does not audit the same held-out examples")
    true_kinds = Counter(row[2] for row in expected)
    majority_kind, majority_correct = true_kinds.most_common(1)[0]
    candidates = {}
    for name, report in reports.items():
        rows = report["rows"]
        candidates[name] = dict(
            checkpoint_sha256=report["checkpoint_sha256"],
            kind_bias_strength=report.get("kind_bias_strength", 0),
            kind_top1=sum(row["predicted_kind"] == row["true_kind"] for row in rows),
            kind_top5=sum(row["kind_rank"] <= 5 for row in rows),
            nonmajority_samples=len(rows) - majority_correct,
            nonmajority_top1=sum(row["true_kind"] != majority_kind and
                                 row["predicted_kind"] == row["true_kind"] for row in rows),
            target_mode_top1=sum(row["target_mode_known"] and
                                 row["predicted_target_mode"] == row["true_target_mode"]
                                 for row in rows),
            actor_top1=sum(row["actor_correct"] for row in rows),
            supported_pair_top1=sum(row["supported_joint_kind"] == row["true_kind"] and
                                     row["supported_joint_target_mode"] == row["true_target_mode"]
                                     for row in rows),
            by_kind={kind: dict(samples=count, correct=sum(row["true_kind"] == kind and
                                                        row["predicted_kind"] == kind for row in rows))
                     for kind, count in sorted(true_kinds.items())})
    return dict(schema="protodd-whole-game-action-comparison-v1",
                sample_mode=reference["sample_mode"],
                validation_identity_sha256=reference["validation_identity_sha256"],
                samples=len(expected), majority_kind=majority_kind,
                majority_correct=majority_correct, candidates=candidates,
                strength_validated=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("audits", nargs="+", help="NAME=PATH/report.json")
    args = parser.parse_args()
    named = {}
    for value in args.audits:
        name, separator, path = value.partition("=")
        if not separator or not name or not path or name in named:
            parser.error("each audit must be a unique NAME=PATH")
        named[name] = Path(path)
    if args.output.exists():
        raise FileExistsError(args.output)
    report = compare(named)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    print(json.dumps(dict(output=str(args.output.resolve()),
                          majority_correct=report["majority_correct"],
                          candidates={name: {field: item[field] for field in
                                             ("kind_top1", "nonmajority_top1", "actor_top1")}
                                      for name, item in report["candidates"].items()}), indent=2))


if __name__ == "__main__":
    main()
