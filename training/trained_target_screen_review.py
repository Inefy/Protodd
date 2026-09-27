"""Review local trained target control against the frozen heuristic build."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def tactical_trace(path: str) -> dict:
    loaded = False
    control = False
    scores = None
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if line.startswith("TACTICAL_TARGET,"):
                loaded = "loaded=1" in line
                control = "control=1" in line
            elif line.startswith("TACTICAL_TARGET_SUMMARY,"):
                parts = dict(part.split("=", 1) for part in line.strip().split(",")[1:]
                             if "=" in part)
                scores = int(parts["candidateScores"])
    return dict(loaded=loaded, control=control, candidate_scores=scores)


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row["tactical"] = tactical_trace(row["log"])
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
    mode = runtime and all(
        not ref[i]["tactical"]["control"] and
        cand[i]["tactical"]["loaded"] and
        cand[i]["tactical"]["control"] and
        cand[i]["tactical"]["candidate_scores"] is not None
        for i in range(n))
    exposure = mode and sum(
        cand[i]["tactical"]["candidate_scores"] > 0 for i in range(n)) >= 2
    result = dict(schema="protodd-trained-target-screen-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime, mode=mode,
                              exposure=exposure),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    army_gains = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(candidate_scores=[cand[i]["tactical"]["candidate_scores"]
                                              for i in range(n)],
                             army_gains_8400=army_gains,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
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
