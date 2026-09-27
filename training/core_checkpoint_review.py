"""Review the frozen PvZ Core checkpoint pilot against its matched reference."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def build_times(path: str) -> dict:
    times = {"core": [], "cannon": []}
    names = {"Protoss_Cybernetics_Core": "core",
             "Protoss_Photon_Cannon": "cannon"}
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("LIFECYCLE,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) >= 6 and fields[2] == "create" and fields[3] == "self":
                key = names.get(fields[5])
                if key:
                    times[key].append(int(fields[1]))
    return {"first_core": times["core"][0] if times["core"] else None,
            "second_cannon": times["cannon"][1] if len(times["cannon"]) >= 2 else None}


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row["build_times"] = build_times(row["log"])
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
    result = dict(schema="protodd-core-checkpoint-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    core_advanced = [i for i in range(n)
                     if cand[i]["build_times"]["first_core"] is not None and
                     ref[i]["build_times"]["first_core"] is not None and
                     cand[i]["build_times"]["first_core"] <=
                     ref[i]["build_times"]["first_core"] - 1000]
    cannon_delay = [cand[i]["build_times"]["second_cannon"] -
                    ref[i]["build_times"]["second_cannon"]
                    if cand[i]["build_times"]["second_cannon"] is not None and
                    ref[i]["build_times"]["second_cannon"] is not None else None
                    for i in range(n)]
    army_gains = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(core_advanced_1000_frames=core_advanced,
                             first_core_reference=[ref[i]["build_times"]["first_core"]
                                                   for i in range(n)],
                             first_core_candidate=[cand[i]["build_times"]["first_core"]
                                                   for i in range(n)],
                             second_cannon_delay=cannon_delay,
                             army_gains_8400=army_gains,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        len(core_advanced) >= 3 and
        all(delay is not None and delay <= 720 for delay in cannon_delay) and
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
