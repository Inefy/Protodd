"""Compare deployable-cadence audits on identical held-out action windows."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

AUDIT_SCHEMA = "protodd-whole-game-cadence-action-audit-v1"


def compare(named_reports):
    if len(named_reports) < 2:
        raise ValueError("at least two cadence audits required")
    reports = {name: json.loads(Path(path).read_text(encoding="utf-8"))
               for name, path in named_reports.items()}
    reference = next(iter(reports.values()))
    if reference.get("schema") != AUDIT_SCHEMA or not reference.get("rows"):
        raise ValueError("missing cadence audit rows")

    def target_key(row):
        return (row["game_id"], row["frame"], row["first_kind"],
                row["first_action_lag"], row["commands_in_window"], row["actor_known"])

    expected = [target_key(row) for row in reference["rows"]]
    for name, report in reports.items():
        if (report.get("schema") != AUDIT_SCHEMA or
                report.get("validation_identity_sha256") != reference["validation_identity_sha256"] or
                report.get("games") != reference["games"] or
                report.get("overall", {}).get("windows") != reference["overall"]["windows"] or
                [target_key(row) for row in report.get("rows", [])] != expected):
            raise ValueError(f"{name} does not audit the same cadence windows")
    kinds = Counter(row[2] for row in expected)
    majority_kind, majority_count = kinds.most_common(1)[0]
    argument_metrics_available = all(
        all("full_signature_known" in row for row in report["rows"])
        for report in reports.values())
    candidates = {}
    for name, report in reports.items():
        rows = report["rows"]
        candidates[name] = dict(
            checkpoint_sha256=report["checkpoint_sha256"],
            first_kind_top1=sum(row["first_kind_correct"] for row in rows),
            nonmajority_top1=sum(row["first_kind"] != majority_kind and
                                 row["first_kind_correct"] for row in rows),
            supported_joint_pair_top1=sum(row["joint_pair_correct"] for row in rows),
            actor_top1=sum(row["actor_correct"] for row in rows),
            actor_known=sum(row["actor_known"] for row in rows),
            by_kind={kind: dict(samples=count, correct=sum(
                row["first_kind"] == kind and row["first_kind_correct"] for row in rows))
                     for kind, count in sorted(kinds.items())})
        if argument_metrics_available:
            candidates[name].update(
                target_mode_top1=sum(row["target_mode_correct"] for row in rows),
                target_entity_known=sum(row["target_entity_known"] for row in rows),
                target_entity_top1=sum(row["target_entity_correct"] for row in rows),
                target_position_known=sum(row["target_position_known"] for row in rows),
                target_position_within_64px=sum(
                    row["target_position_known"] and row["target_position_error_px"] <= 64
                    for row in rows),
                unit_type_known=sum(row["unit_type_known"] for row in rows),
                unit_type_top1=sum(row["unit_type_correct"] for row in rows),
                full_signature_known=sum(row["full_signature_known"] for row in rows),
                full_signature_top1=sum(row["full_signature_correct"] for row in rows))
    return dict(schema="protodd-whole-game-cadence-comparison-v1",
                validation_identity_sha256=reference["validation_identity_sha256"],
                games=reference["games"], windows=reference["overall"]["windows"],
                action_windows=len(expected), majority_kind=majority_kind,
                majority_kind_correct=majority_count,
                argument_metrics_available=argument_metrics_available,
                candidates=candidates,
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
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(output=str(args.output.resolve()),
                          action_windows=report["action_windows"],
                          majority_kind_correct=report["majority_kind_correct"],
                          candidates={name: {metric: result[metric] for metric in
                                             (("first_kind_top1", "nonmajority_top1",
                                               "supported_joint_pair_top1", "actor_top1") +
                                              (("full_signature_top1",) if report[
                                                  "argument_metrics_available"] else ())) }
                                      for name, result in report["candidates"].items()}), indent=2))


if __name__ == "__main__":
    main()
