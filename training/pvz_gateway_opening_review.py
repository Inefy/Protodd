"""Review the frozen gateway-first PvZ development pilot."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def first_zealot(path):
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if (len(fields) > 5 and fields[0] == "LIFECYCLE" and
                    fields[2:4] == ["complete", "self"] and
                    fields[5] == "Protoss_Zealot"):
                return int(fields[1])
    return None


def review(plan_path, reference_path, candidate_path):
    plan_path = Path(plan_path)
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row["first_zealot"] = first_zealot(row["log"])
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
        row["first_zealot"] is not None and "6000" in row["states"]
        for row in (*ref.values(), *cand.values()))
    checks = dict(healthy=healthy, identical_inputs=identical_inputs,
                  paired=paired, runtime=runtime)
    result = dict(schema="protodd-pvz-gateway-opening-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate, checks=checks,
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(checks.values()):
        return result
    defender_gains = [ref[i]["first_zealot"] - cand[i]["first_zealot"]
                      for i in range(n)]
    army_gains = [cand[i]["states"]["6000"]["army"] -
                  ref[i]["states"]["6000"]["army"] for i in range(n)]
    probe_gains = [cand[i]["states"]["6000"]["probes"] -
                   ref[i]["states"]["6000"]["probes"] for i in range(n)]
    extra_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                    for i in range(n)]
    core_preserved = all(
        cand[i]["first_core"] is not None and cand[i]["first_core"] <= 12000
        for i in range(n) if ref[i]["first_core"] is not None and
        ref[i]["first_core"] <= 12000)
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(defender_gains=defender_gains,
                             army_gains_6000=army_gains,
                             probe_gains_6000=probe_gains,
                             extra_early_losses=extra_losses,
                             core_preserved=core_preserved,
                             wins_reference=wins_ref,
                             wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(defender_gains) / n >= 480 and
        sum(army_gains) / n >= 2 and
        sum(probe_gains) / n >= -2 and
        sum(extra_losses) <= 1 and core_preserved and
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
