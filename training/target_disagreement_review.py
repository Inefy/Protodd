"""Summarize live target choices against the same-set heuristic comparator."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def counters(path: str) -> dict:
    result = None
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if line.startswith("TACTICAL_TARGET_SUMMARY,"):
                result = dict(part.split("=", 1) for part in line.strip().split(",")[1:]
                              if "=" in part)
    if result is None:
        return {}
    return {key: int(value) for key, value in result.items()}


def review(plan_path: Path, run_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    run = campaign(run_path, plan["games"])
    rows = sorted(run["rows"], key=lambda row: row["game_id"])
    for row in rows:
        row["target_counters"] = counters(row["log"])
    names = ("candidateScores", "comparisons", "disagreements",
             "workerOverCombat", "buildingOverCombat", "combatOverWorker",
             "combatOverBuilding", "threatAbandoned")
    valid = run["healthy"] and run["dll_sha256"] == plan["dll_sha256"] and all(
        row["enemy_activity"] and not row["errors"] and
        row["target_counters"].get("control") == 1 and
        all(name in row["target_counters"] for name in names)
        for row in rows)
    totals = {name: sum(row["target_counters"].get(name, 0) for row in rows)
              for name in names}
    exposure = valid and all(row["target_counters"]["comparisons"] > 1000
                             for row in rows)
    return dict(schema="protodd-target-disagreement-review-v1",
                plan_sha256=sha256(plan_path),
                review_source_sha256=sha256(__file__),
                run=run, checks=dict(valid=valid, exposure=exposure),
                totals=totals,
                disagreement_rate=(totals["disagreements"] / totals["comparisons"]
                                   if totals["comparisons"] else None),
                promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("run", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.run)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in
                      ("checks", "totals", "disagreement_rate")}))
