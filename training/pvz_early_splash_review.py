"""Review the isolated evidence-triggered PvZ Reaver transition."""

import argparse
import json
import re
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def timing_trace(path: str) -> dict:
    result = dict(first_hydra=None, first_signal=None, first_muta=None,
                  first_spire=None, demand=None,
                  core_complete=None, robotics_create=None,
                  support_create=None, reaver_create=None, reaver_complete=None)
    lifecycle = {
        ("complete", "Protoss_Cybernetics_Core"): "core_complete",
        ("create", "Protoss_Robotics_Facility"): "robotics_create",
        ("create", "Protoss_Robotics_Support_Bay"): "support_create",
        ("create", "Protoss_Reaver"): "reaver_create",
        ("complete", "Protoss_Reaver"): "reaver_complete",
    }
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if line.startswith("STATE,"):
                fields = line.rstrip("\n").split(",", 2)
                frame = int(fields[1])
                if result["first_hydra"] is None and "Hydralisk=" in line:
                    result["first_hydra"] = frame
                hydra = re.search(r"(?:^|;)Hydralisk=(\d+)", line)
                if result["first_signal"] is None and (
                        "Hydralisk Den@" in line or
                        (hydra is not None and int(hydra.group(1)) >= 2)):
                    result["first_signal"] = frame
                if result["first_muta"] is None and "Mutalisk=" in line:
                    result["first_muta"] = frame
                if result["first_spire"] is None and "Spire=" in line:
                    result["first_spire"] = frame
                if result["demand"] is None and (
                        "[early Reaver screen]" in line or
                        "start splash production on first Hydra evidence" in line):
                    result["demand"] = frame
            elif line.startswith("LIFECYCLE,"):
                fields = line.rstrip("\n").split(",")
                if len(fields) > 5 and fields[3] == "self":
                    key = lifecycle.get((fields[2], fields[5]))
                    if key is not None and result[key] is None:
                        result[key] = int(fields[1])
    return result


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(timing_trace(row["log"]))
    healthy = reference["healthy"] and candidate["healthy"]
    identical_inputs = (
        reference["common_inputs_sha256"] == candidate["common_inputs_sha256"] and
        reference["dll_sha256"] == plan["reference_dll_sha256"] and
        candidate["dll_sha256"] == plan["candidate_dll_sha256"])
    paired = healthy and all(
        ref[i]["match"] == cand[i]["match"] and
        ref[i]["host"] == cand[i]["host"] and ref[i]["map"] == cand[i]["map"]
        for i in range(n))
    runtime = paired and all(
        row["enemy_activity"] and not row["errors"] and
        "8400" in row["states"] for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-pvz-early-splash-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    exposed = [cand[i]["first_signal"] is not None and
               cand[i]["first_signal"] <= 11000 and
               (cand[i]["first_muta"] is None or
                cand[i]["first_signal"] < cand[i]["first_muta"]) and
               (cand[i]["first_spire"] is None or
                cand[i]["first_signal"] < cand[i]["first_spire"])
               for i in range(n)]
    early_robo = [cand[i]["robotics_create"] is not None and
                  cand[i]["robotics_create"] <= 12000 for i in range(n)]
    early_reaver = [cand[i]["reaver_create"] is not None and
                    cand[i]["reaver_create"] <= 13000 for i in range(n)]
    army_delta = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(hydra_exposure=exposed,
                             first_hydra_reference=[ref[i]["first_hydra"] for i in range(n)],
                             first_hydra_candidate=[cand[i]["first_hydra"] for i in range(n)],
                             first_signal_candidate=[cand[i]["first_signal"] for i in range(n)],
                             first_muta_candidate=[cand[i]["first_muta"] for i in range(n)],
                             first_spire_candidate=[cand[i]["first_spire"] for i in range(n)],
                             candidate_demand=[cand[i]["demand"] for i in range(n)],
                             core_complete_reference=[ref[i]["core_complete"] for i in range(n)],
                             core_complete_candidate=[cand[i]["core_complete"] for i in range(n)],
                             robotics_create_reference=[ref[i]["robotics_create"] for i in range(n)],
                             robotics_create_candidate=[cand[i]["robotics_create"] for i in range(n)],
                             support_create_candidate=[cand[i]["support_create"] for i in range(n)],
                             reaver_create_reference=[ref[i]["reaver_create"] for i in range(n)],
                             reaver_create_candidate=[cand[i]["reaver_create"] for i in range(n)],
                             reaver_complete_candidate=[cand[i]["reaver_complete"] for i in range(n)],
                             army_delta_8400=army_delta,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(exposed) >= 2 and
        sum(cand[i]["demand"] is not None for i in range(n) if exposed[i]) >= 2 and
        sum(early_robo[i] for i in range(n) if exposed[i]) >= 2 and
        sum(early_reaver[i] for i in range(n) if exposed[i]) >= 1 and
        sum(army_delta) / n >= -1 and sum(extra_early_losses) <= 1 and
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
