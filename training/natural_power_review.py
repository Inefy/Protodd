"""Review the frozen early natural-power development comparison."""

import argparse
import json
from pathlib import Path

from .powered_defense_review import placement_trace
from .pvz_mobile_screen_review import campaign
from .schema import sha256


def natural_trace(path: str) -> dict:
    nexuses = []
    pylons = []
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("LIFECYCLE,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) < 8 or fields[2:4] != ["create", "self"]:
                continue
            if fields[5] not in ("Protoss_Nexus", "Protoss_Pylon"):
                continue
            row = (int(fields[1]), int(fields[6][2:]), int(fields[7][2:]))
            (nexuses if fields[5] == "Protoss_Nexus" else pylons).append(row)
    natural = nexuses[1] if len(nexuses) > 1 else None
    nearby_pylons = [row for row in pylons if natural is not None and
                     (row[1] - natural[1]) ** 2 +
                     (row[2] - natural[2]) ** 2 <= 384 ** 2]
    return dict(natural_nexus_create=natural[0] if natural else None,
                natural_pylon_create=min((row[0] for row in nearby_pylons),
                                         default=None))


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(natural_trace(row["log"]))
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
        "8400" in row["states"] and row["natural_nexus_create"] is not None
        for row in (*ref.values(), *cand.values()))
    result = dict(schema="protodd-natural-power-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    pylon_ref = [ref[i]["natural_pylon_create"] for i in range(n)]
    pylon_cand = [cand[i]["natural_pylon_create"] for i in range(n)]
    earlier = [pylon_cand[i] is not None and
               (pylon_ref[i] is None or pylon_cand[i] <= pylon_ref[i] - 1000)
               for i in range(n)]
    cannon_ref = [ref[i]["second_cannon_create"] for i in range(n)]
    cannon_cand = [cand[i]["second_cannon_create"] for i in range(n)]
    army_gains = [cand[i]["states"]["8400"]["army"] -
                  ref[i]["states"]["8400"]["army"] for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(natural_pylon_reference=pylon_ref,
                             natural_pylon_candidate=pylon_cand,
                             natural_power_advanced=earlier,
                             second_cannon_reference=cannon_ref,
                             second_cannon_candidate=cannon_cand,
                             army_gains_8400=army_gains,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(earlier) >= 3 and all(pylon is not None for pylon in pylon_cand) and
        all(cannon_cand[i] is not None and cannon_ref[i] is not None and
            cannon_cand[i] <= cannon_ref[i] + 720 for i in range(n)) and
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
