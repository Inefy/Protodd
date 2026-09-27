"""Review the frozen six-Zealot PvZ pressure pilot against its matched control."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def pressure_events(path: str) -> dict:
    first_plan = None
    first_forward_squad = None
    economic_losses = 0
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if len(fields) < 3:
                continue
            try:
                frame = int(fields[1])
            except ValueError:
                continue
            if fields[0] == "STRATEGY" and "[six-Zealot expansion pressure]" in line:
                first_plan = frame if first_plan is None else first_plan
            elif fields[0] == "SQUAD" and first_forward_squad is None and "travelReason=attack-target" in line:
                unit_field = next((part for part in fields if part.startswith("units=")), "units=0")
                if fields[2] == "MainArmy" and int(unit_field.split("=", 1)[1]) >= 6:
                    first_forward_squad = frame
            elif (fields[0] == "LOSS" and len(fields) > 4 and
                  fields[2] == "enemy" and fields[4] in ("Drone", "Hatchery") and
                  frame <= 10500):
                economic_losses += 1
    return dict(first_pressure_plan=first_plan,
                first_forward_squad=first_forward_squad,
                enemy_economic_losses_10500=economic_losses)


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(pressure_events(row["log"]))
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
        "6000" in row["states"] for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-pvz-pressure-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    forward = [cand[i]["first_forward_squad"] for i in range(n)]
    army_gains = [cand[i]["states"]["6000"]["army"] -
                  ref[i]["states"]["6000"]["army"] for i in range(n)]
    extra_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                    for i in range(n)]
    economic_gains = [cand[i]["enemy_economic_losses_10500"] -
                      ref[i]["enemy_economic_losses_10500"] for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(first_forward_squad=forward,
                             army_gains_6000=army_gains,
                             extra_early_losses=extra_losses,
                             enemy_economic_loss_gains_10500=economic_gains,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(frame is not None and frame <= 7800 for frame in forward) >= 3 and
        sum(army_gains) / n >= -1 and
        sum(extra_losses) <= 1 and
        sum(economic_gains) >= 2 and
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
