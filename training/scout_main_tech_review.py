"""Review a frozen main-base tech-scout development comparison."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .schema import sha256


def scout_trace(path: str) -> dict:
    main = None
    main_revisit = None
    first_tech = None
    followup_dispatched = False
    probe_losses_8400 = 0
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if fields[0] == "STATE":
                frame = int(fields[1])
                structures = next((field.partition("=")[2] for field in fields
                                   if field.startswith("knownStructures=")), "")
                for item in structures.split(";"):
                    if not item or "@" not in item or ":" not in item:
                        continue
                    kind, _, rest = item.partition("@")
                    position, _, seen = rest.partition(":")
                    if kind == "Hatchery" and main is None and frame < 5000:
                        main = position
                    if (kind in ("Hydralisk Den", "Spire") and
                            first_tech is None):
                        first_tech = frame
                    if (main is not None and position == main and frame >= 5000 and
                            int(seen) >= 5000 and main_revisit is None):
                        main_revisit = frame
            elif fields[0] == "WORKERS":
                frame = int(fields[1])
                followup_dispatched |= (5000 <= frame < 8000 and
                                        "scouts=1" in fields)
            elif (fields[0] == "LOSS" and len(fields) > 4 and
                  fields[2] == "self" and fields[4] == "Probe" and
                  int(fields[1]) < 8400):
                probe_losses_8400 += 1
    return dict(enemy_main=main, main_revisit=main_revisit,
                first_den_or_spire=first_tech,
                followup_dispatched=followup_dispatched,
                probe_losses_8400=probe_losses_8400)


def review(plan_path: Path, reference_path: Path, candidate_path: Path) -> dict:
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    for rows in (ref, cand):
        for row in rows.values():
            row.update(scout_trace(row["log"]))
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
    result = dict(schema="protodd-scout-main-tech-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    dispatch = [cand[i]["followup_dispatched"] for i in range(n)]
    timely = [cand[i]["main_revisit"] is not None and
              cand[i]["main_revisit"] < 8000 for i in range(n)]
    earlier_tech = [cand[i]["first_den_or_spire"] is not None and
                    (ref[i]["first_den_or_spire"] is None or
                     cand[i]["first_den_or_spire"] < ref[i]["first_den_or_spire"])
                    for i in range(n)]
    loss_delta = [cand[i]["probe_losses_8400"] - ref[i]["probe_losses_8400"]
                  for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(main_revisit_reference=[ref[i]["main_revisit"] for i in range(n)],
                             main_revisit_candidate=[cand[i]["main_revisit"] for i in range(n)],
                             followup_dispatched=dispatch, timely_main_revisit=timely,
                             first_tech_reference=[ref[i]["first_den_or_spire"] for i in range(n)],
                             first_tech_candidate=[cand[i]["first_den_or_spire"] for i in range(n)],
                             earlier_tech=earlier_tech, extra_probe_losses_8400=loss_delta,
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(timely[i] for i in range(n) if dispatch[i]) >= 2 and
        sum(loss_delta) <= 1 and sum(earlier_tech) >= 2 and
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
