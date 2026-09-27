"""Review the frozen PvZ Core-before-extra-Zealots pilot."""

import argparse
import json
from pathlib import Path

from .fortified_zealot_review import review as zealot_review
from .schema import sha256


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    result = zealot_review(plan_path, reference_path, candidate_path)
    result["schema"] = "protodd-balanced-fortress-review-v1"
    result["review_source_sha256"] = sha256(__file__)
    if not all(result["checks"].values()):
        return result
    ref = {row["game_id"]: row for row in result["reference"]["rows"]}
    cand = {row["game_id"]: row for row in result["candidate"]["rows"]}
    n = len(ref)
    core_delay = [cand[i]["build_times"]["first_core"] -
                  ref[i]["build_times"]["first_core"]
                  if cand[i]["build_times"]["first_core"] is not None and
                  ref[i]["build_times"]["first_core"] is not None else None
                  for i in range(n)]
    result["metrics"]["core_delay"] = core_delay
    result["functional_pass"] = result["functional_pass"] and all(
        delay is not None and delay <= 720 for delay in core_delay)
    result["larger_campaign"] = (
        result["functional_pass"] and
        result["metrics"]["wins_candidate"] > result["metrics"]["wins_reference"])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.reference, args.candidate)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result.get(key) for key in
                      ("checks", "metrics", "functional_pass", "larger_campaign")}))
