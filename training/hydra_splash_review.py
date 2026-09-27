"""Review the frozen Hydra splash development comparison."""

import argparse
import json
import re
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


OBSERVER_GOAL = re.compile(r"(?:^|;)1:Observer:\d+:\d+:B:")


def splash_trace(path: str) -> dict:
    first = {name: None for name in (
        "Robotics_Facility", "Robotics_Support_Bay", "Reaver",
        "Observatory", "Observer")}
    unsupported_detection = 0
    hydra_states = 0
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if line.startswith("LIFECYCLE,"):
                fields = line.rstrip("\n").split(",")
                if len(fields) > 5 and fields[2] == "create" and fields[3] == "self":
                    kind = fields[5].removeprefix("Protoss_")
                    if kind in first and first[kind] is None:
                        first[kind] = int(fields[1])
            elif line.startswith("STATE,") and "[anti-hydra splash]" in line:
                hydra_states += 1
                goals = line.partition(",goals=")[2].partition(",actionDetail=")[0]
                enemy = line.partition(",enemyComp=")[2].partition(",selfComp=")[0]
                if "Lurker=" not in enemy and OBSERVER_GOAL.search(goals):
                    unsupported_detection += 1
    return dict(first_create=first, hydra_states=hydra_states,
                unsupported_detection=unsupported_detection)


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(splash_trace(row["log"]))
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
        "8400" in row["states"] and row["hydra_states"] > 0
        for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-hydra-splash-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result

    def splash_start(row: dict) -> int | None:
        created = row["first_create"]
        times = [created[name] for name in ("Robotics_Support_Bay", "Reaver")
                 if created[name] is not None]
        return min(times) if times else None

    splash_reference = [splash_start(ref[i]) for i in range(n)]
    splash_candidate = [splash_start(cand[i]) for i in range(n)]
    advanced = [splash_candidate[i] is not None and
                (splash_reference[i] is None or
                 splash_candidate[i] <= splash_reference[i] - 240)
                for i in range(n)]
    unsupported_ref = [ref[i]["unsupported_detection"] for i in range(n)]
    unsupported_cand = [cand[i]["unsupported_detection"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(splash_first_reference=splash_reference,
                             splash_first_candidate=splash_candidate,
                             splash_advanced=advanced,
                             unsupported_detection_reference=unsupported_ref,
                             unsupported_detection_candidate=unsupported_cand,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(advanced) >= 2 and sum(unsupported_ref) > 0 and
        sum(unsupported_cand) == 0 and sum(extra_early_losses) <= 1 and
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
