"""Review the frozen early third-Cannon PvZ pilot."""

import argparse
import json
from pathlib import Path

from .core_checkpoint_review import review as core_review
from .schema import sha256


def third_cannon_time(path: str) -> int | None:
    times = []
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("LIFECYCLE,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) >= 6 and fields[2] == "create" and \
                    fields[3] == "self" and fields[5] == "Protoss_Photon_Cannon":
                times.append(int(fields[1]))
    return times[2] if len(times) >= 3 else None


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    result = core_review(plan_path, reference_path, candidate_path)
    result["schema"] = "protodd-third-cannon-review-v1"
    result["review_source_sha256"] = sha256(__file__)
    if not all(result["checks"].values()):
        return result
    ref = {row["game_id"]: row for row in result["reference"]["rows"]}
    cand = {row["game_id"]: row for row in result["candidate"]["rows"]}
    n = len(ref)
    ref_third = [third_cannon_time(ref[i]["log"]) for i in range(n)]
    cand_third = [third_cannon_time(cand[i]["log"]) for i in range(n)]
    core_delay = [cand[i]["build_times"]["first_core"] -
                  ref[i]["build_times"]["first_core"]
                  if cand[i]["build_times"]["first_core"] is not None and
                  ref[i]["build_times"]["first_core"] is not None else None
                  for i in range(n)]
    metrics = result["metrics"]
    metrics["third_cannon_reference"] = ref_third
    metrics["third_cannon_candidate"] = cand_third
    metrics["core_delay"] = core_delay
    result["functional_pass"] = (
        sum(time is not None and time <= 9500 for time in cand_third) >= 3 and
        all(delay is not None and delay <= 720
            for delay in metrics["second_cannon_delay"]) and
        all(delay is not None and delay <= 1500 for delay in core_delay) and
        sum(metrics["army_gains_8400"]) / n >= -1 and
        sum(metrics["extra_early_losses"]) <= 1 and
        metrics["wins_candidate"] >= metrics["wins_reference"])
    result["larger_campaign"] = (
        result["functional_pass"] and
        metrics["wins_candidate"] > metrics["wins_reference"])
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
