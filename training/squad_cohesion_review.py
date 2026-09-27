"""Review the frozen squad-cohesion development comparison."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def unsupported_forward_rows(path: str) -> int:
    count = 0
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("SQUAD,") or "travelReason=attack-target" not in line:
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) < 3 or fields[2] != "MainArmy":
                continue
            members = next((int(part[6:]) for part in fields
                            if part.startswith("units=")), 0)
            enemies = next((int(part[8:]) for part in fields
                            if part.startswith("enemies=")), 0)
            if members < 6 and enemies > 0:
                count += 1
    return count


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row["unsupported_forward_rows"] = unsupported_forward_rows(row["log"])
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
        "8400" in row["states"] for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-squad-cohesion-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    unsupported_ref = [ref[i]["unsupported_forward_rows"] for i in range(n)]
    unsupported_cand = [cand[i]["unsupported_forward_rows"] for i in range(n)]
    early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                    for i in range(n)]
    army_gains = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(unsupported_forward_reference=unsupported_ref,
                             unsupported_forward_candidate=unsupported_cand,
                             extra_early_losses=early_losses,
                             army_gains_8400=army_gains,
                             wins_reference=wins_ref,
                             wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(unsupported_ref) > 0 and sum(unsupported_cand) == 0 and
        sum(early_losses) <= 1 and sum(army_gains) / n >= -2 and
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
