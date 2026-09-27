"""Review the frozen powered-defense placement development comparison."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def placement_trace(path: str) -> dict:
    no_location = 0
    cannon_creates = []
    second_cannon_demand = False
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if line.startswith("MACRO,"):
                fields = line.rstrip("\n").split(",")
                if (len(fields) > 5 and fields[3] == "Photon Cannon" and
                        fields[5].startswith("build-no-location-")):
                    no_location += 1
            elif line.startswith("LIFECYCLE,"):
                fields = line.rstrip("\n").split(",")
                if (len(fields) > 5 and fields[2:4] == ["create", "self"] and
                        fields[5] == "Protoss_Photon_Cannon"):
                    cannon_creates.append(int(fields[1]))
            elif line.startswith("STATE,") and "Photon Cannon:2:" in line:
                goals = line.partition(",goals=")[2].partition(",actionDetail=")[0]
                second_cannon_demand |= "Photon Cannon:2:" in goals
    return dict(no_location=no_location,
                second_cannon_create=cannon_creates[1] if len(cannon_creates) > 1 else None,
                second_cannon_demand=second_cannon_demand)


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(placement_trace(row["log"]))
    healthy = reference["healthy"] and candidate["healthy"]
    identical_inputs = (reference["common_inputs_sha256"] ==
                        candidate["common_inputs_sha256"] and
                        reference["dll_sha256"] == plan["reference_dll_sha256"] and
                        candidate["dll_sha256"] == plan["candidate_dll_sha256"])
    paired = healthy and all(
        ref[i]["match"] == cand[i]["match"] and
        ref[i]["host"] == cand[i]["host"] and
        ref[i]["map"] == cand[i]["map"] and
        int(ref[i]["match"]["seed"]) == plan["seed_base"] + i
        for i in range(n))
    runtime = paired and all(
        row["enemy_activity"] and not row["errors"] and
        "8400" in row["states"] and row["second_cannon_demand"]
        for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-powered-defense-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    no_location_ref = [ref[i]["no_location"] for i in range(n)]
    no_location_cand = [cand[i]["no_location"] for i in range(n)]
    second_ref = [ref[i]["second_cannon_create"] for i in range(n)]
    second_cand = [cand[i]["second_cannon_create"] for i in range(n)]
    faster = [second_cand[i] is not None and second_ref[i] is not None and
              second_cand[i] <= second_ref[i] - 480 for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(no_location_reference=no_location_ref,
                             no_location_candidate=no_location_cand,
                             second_cannon_reference=second_ref,
                             second_cannon_candidate=second_cand,
                             second_cannon_at_least_480_earlier=faster,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(no_location_cand) * 2 <= sum(no_location_ref) and
        sum(faster) >= 3 and sum(extra_early_losses) <= 1 and
        wins_cand >= wins_ref)
    result["larger_campaign"] = result["functional_pass"] and wins_cand > wins_ref
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
