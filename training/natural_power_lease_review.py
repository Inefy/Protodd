"""Review the scoped natural-Pylon travel-lease development comparison."""

import argparse
import json
from pathlib import Path

from .natural_power_review import review as base_review
from .schema import sha256


def remote_pylon_expiries(path: str) -> int:
    count = 0
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("BUILDLEASE,") or "kind=Protoss_Pylon" not in line or \
                    "reason=hard-lease-limit" not in line:
                continue
            frame = int(line.split(",", 2)[1])
            if 6000 <= frame < 10000:
                count += 1
    return count


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    result = base_review(plan_path, reference_path, candidate_path)
    result["schema"] = "protodd-natural-power-lease-review-v1"
    result["review_source_sha256"] = sha256(__file__)
    if not all(result["checks"].values()):
        return result
    ref = sorted(result["reference"]["rows"], key=lambda row: row["game_id"])
    cand = sorted(result["candidate"]["rows"], key=lambda row: row["game_id"])
    expiries_ref = [remote_pylon_expiries(row["log"]) for row in ref]
    expiries_cand = [remote_pylon_expiries(row["log"]) for row in cand]
    result["metrics"]["pylon_hard_expiries_reference"] = expiries_ref
    result["metrics"]["pylon_hard_expiries_candidate"] = expiries_cand
    result["functional_pass"] = (result["functional_pass"] and
                                  sum(expiries_ref) > 0 and
                                  sum(expiries_cand) * 3 <= sum(expiries_ref))
    result["larger_campaign"] = (result["functional_pass"] and
                                 result["metrics"]["wins_candidate"] >
                                 result["metrics"]["wins_reference"])
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
