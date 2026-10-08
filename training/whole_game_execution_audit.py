"""Summarize learned-action loss between proposal and native command observation."""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
import csv
import hashlib
import json
from pathlib import Path


SCHEMA = "protodd-whole-game-execution-audit-v1"
REQUIRED_COLUMNS = {
    "attempt_id", "proposal_frame", "slot", "due_frame", "frame",
    "actor_token", "actor_ordinal", "actor_count", "intent_kind",
    "target_mode", "target_entity", "target_x", "target_y", "stage",
    "outcome", "reason",
}


def load_events(path: Path) -> list[dict]:
    """Read and validate the runtime's per-actor event stream."""
    events = []
    with Path(path).open("r", encoding="utf-8-sig", newline="") as source:
        reader = csv.DictReader(source)
        if reader.fieldnames is None or not REQUIRED_COLUMNS.issubset(reader.fieldnames):
            missing = sorted(REQUIRED_COLUMNS - set(reader.fieldnames or ()))
            raise ValueError(f"action audit is missing required columns: {missing}")
        for line, row in enumerate(reader, start=2):
            if None in row or any(row.get(column) is None for column in REQUIRED_COLUMNS):
                raise ValueError(f"malformed CSV row at line {line}")
            try:
                event = dict(row)
                for column in ("attempt_id", "proposal_frame", "slot", "due_frame",
                               "frame", "actor_token", "actor_ordinal", "actor_count",
                               "intent_kind", "target_mode", "target_entity", "target_x",
                               "target_y"):
                    event[column] = int(row[column])
            except (TypeError, ValueError) as error:
                raise ValueError(f"invalid numeric field at line {line}") from error
            if not event["stage"] or not event["outcome"]:
                raise ValueError(f"empty stage or outcome at line {line}")
            events.append(event)
    return events


def summarize(events: list[dict]) -> dict:
    """Report actor-level stage transitions and the reasons commands were lost."""
    actors = defaultdict(lambda: defaultdict(set))
    row_counts = Counter()
    loss_reasons = Counter()
    for event in events:
        key = (event["attempt_id"], event["actor_ordinal"], event["actor_token"])
        stage, outcome = event["stage"], event["outcome"]
        actors[key][stage].add(outcome)
        row_counts[(stage, outcome)] += 1
        if outcome in {"rejected", "deferred", "canceled", "unobserved", "censored"}:
            loss_reasons[(stage, outcome, event["reason"])] += 1

    proposed = {key for key, stages in actors.items() if "emitted" in stages["proposal"]}
    legal = {key for key, stages in actors.items() if "legal" in stages["legality"]}
    rejected = {key for key, stages in actors.items()
                if "rejected" in stages["legality"] and key not in legal}
    selected = {key for key, stages in actors.items() if "selected" in stages["arbitration"]}
    api_accepted = {key for key, stages in actors.items() if "accepted" in stages["api"]}
    api_rejected = {key for key, stages in actors.items() if "rejected" in stages["api"]}
    execution_observed = {key for key, stages in actors.items()
                          if "observed" in stages["execution"]}
    execution_unobserved = {key for key, stages in actors.items()
                            if "unobserved" in stages["execution"]}
    execution_censored = {key for key, stages in actors.items()
                          if "censored" in stages["execution"]}
    execution_terminal = execution_observed | execution_unobserved | execution_censored
    pending_execution = api_accepted - execution_terminal
    legal_not_selected = legal - selected
    selected_not_accepted = selected & api_rejected
    selected_awaiting_api = selected - api_accepted - api_rejected

    def rate(numerator, denominator):
        return numerator / denominator if denominator else None

    summary = {
        "schema": SCHEMA,
        "event_rows": len(events),
        "actor_attempts": {
            "proposed": len(proposed),
            "legal": len(legal),
            "legality_rejected": len(rejected),
            "selected": len(selected),
            "api_accepted": len(api_accepted),
            "api_rejected": len(api_rejected),
            "execution_observed": len(execution_observed),
            "execution_unobserved": len(execution_unobserved),
            "execution_censored": len(execution_censored),
            "accepted_pending_execution": len(pending_execution),
            "legal_not_selected": len(legal_not_selected),
            "selected_not_accepted": len(selected_not_accepted),
            "selected_awaiting_api": len(selected_awaiting_api),
        },
        "rates": {
            "proposal_to_legal": rate(len(proposed & legal), len(proposed)),
            "legal_to_selected": rate(len(legal & selected), len(legal)),
            "selected_to_api_accepted": rate(len(selected & api_accepted), len(selected)),
            "accepted_execution_coverage": rate(len(execution_terminal), len(api_accepted)),
            "observed_among_uncensored_terminal": rate(
                len(execution_observed), len(execution_observed | execution_unobserved)),
        },
        "event_counts": {
            f"{stage}:{outcome}": count
            for (stage, outcome), count in sorted(row_counts.items())
        },
        "loss_reasons": [
            {"stage": stage, "outcome": outcome, "reason": reason, "events": count}
            for (stage, outcome, reason), count in sorted(loss_reasons.items())
        ],
        "measurement_note": (
            "execution_observed means BWAPI getLastCommand matched the accepted order; "
            "it does not prove that the order achieved its in-game objective."
        ),
    }
    return summary


def audit(input_path: Path) -> dict:
    input_path = Path(input_path)
    events = load_events(input_path)
    result = summarize(events)
    result["input_sha256"] = hashlib.sha256(input_path.read_bytes()).hexdigest()
    return result


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="WholeGame-actions.csv from a native run")
    parser.add_argument("--output", type=Path, help="write a JSON report (must not exist)")
    args = parser.parse_args()
    report = audit(args.input)
    rendered = json.dumps(report, indent=2) + "\n"
    if args.output:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
