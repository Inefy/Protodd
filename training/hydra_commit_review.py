"""Review the frozen scouted-Hydra splash commitment pilot."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def tech_trace(path: str) -> dict:
    first = {name: None for name in (
        "hydralisk", "hydraliskDen", "roboticsFacility",
        "roboticsSupportBay", "reaver", "stargate", "corsair")}
    names = {
        "Zerg_Hydralisk": "hydralisk",
        "Zerg_Hydralisk_Den": "hydraliskDen",
        "Protoss_Robotics_Facility": "roboticsFacility",
        "Protoss_Robotics_Support_Bay": "roboticsSupportBay",
        "Protoss_Reaver": "reaver",
        "Protoss_Stargate": "stargate",
        "Protoss_Corsair": "corsair",
    }
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("LIFECYCLE,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) < 6 or fields[5] not in names:
                continue
            key = names[fields[5]]
            if first[key] is not None:
                continue
            if (fields[3] == "self" and fields[2] == "create") or \
                    (fields[3] == "enemy" and fields[2] in ("discover", "complete")):
                first[key] = int(fields[1])
    return first


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row["tech"] = tech_trace(row["log"])
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
        "8400" in row["states"]
        for row in (*ref.values(), *cand.values()))
    exposure = runtime and sum(row["tech"]["hydralisk"] is not None
                               for row in ref.values()) >= 2 and \
        sum(row["tech"]["hydralisk"] is not None for row in cand.values()) >= 2
    result = dict(schema="protodd-hydra-commit-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime, exposure=exposure),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    exposure_ids = [i for i in range(n) if cand[i]["tech"]["hydralisk"] is not None]
    reaver_on_time = [i for i in exposure_ids
                      if cand[i]["tech"]["reaver"] is not None and
                      cand[i]["tech"]["reaver"] <=
                      cand[i]["tech"]["hydralisk"] + 4800]
    support_advanced = [i for i in exposure_ids
                        if cand[i]["tech"]["roboticsSupportBay"] is not None and
                        (ref[i]["tech"]["roboticsSupportBay"] is None or
                         cand[i]["tech"]["roboticsSupportBay"] <=
                         ref[i]["tech"]["roboticsSupportBay"] - 720)]
    army_gains = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(hydra_exposure_candidate=exposure_ids,
                             reaver_within_4800_frames=reaver_on_time,
                             support_bay_advanced=support_advanced,
                             army_gains_8400=army_gains,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        len(reaver_on_time) >= 2 and len(support_advanced) >= 2 and
        sum(army_gains) / n >= -1 and sum(extra_early_losses) <= 1 and
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
