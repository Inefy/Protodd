"""Review natural selection and timed-out Probe return in matched games."""

import argparse
import json
from pathlib import Path

from .pvz_mobile_screen_review import campaign
from .scout_main_tech_review import scout_trace
from .schema import sha256


def return_trace(path: str) -> dict:
    return_frames = set()
    last_scout_frame = {}
    cluster_start = {}
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("SCOUT,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) < 4:
                continue
            frame = int(fields[1])
            actor = fields[2]
            if actor not in last_scout_frame or frame - last_scout_frame[actor] > 600:
                cluster_start[actor] = frame
            last_scout_frame[actor] = frame
            # Opening withdrawal can last past frame 5000. A later command
            # cluster begins only after the opening scout has been released.
            if fields[3] == "probe-harass-withdraw" and cluster_start[actor] >= 6000:
                return_frames.add(frame)
    return dict(followup_return_orders=len(return_frames),
                first_followup_return=min(return_frames, default=None))


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
            row.update(return_trace(row["log"]))
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
    result = dict(schema="protodd-scout-safe-return-review-v1",
                  plan_sha256=sha256(plan_path),
                  review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate,
                  checks=dict(healthy=healthy, identical_inputs=identical_inputs,
                              paired=paired, runtime=runtime),
                  functional_pass=False, larger_campaign=False,
                  promotion_allowed=False)
    if not all(result["checks"].values()):
        return result
    returns = [cand[i]["followup_return_orders"] for i in range(n)]
    extra_probe_losses = [cand[i]["probe_losses_8400"] -
                          ref[i]["probe_losses_8400"] for i in range(n)]
    wins_ref = sum(ref[i]["won"] for i in range(n))
    wins_cand = sum(cand[i]["won"] for i in range(n))
    result["metrics"] = dict(followup_return_orders=returns,
                             first_followup_return=[cand[i]["first_followup_return"]
                                                    for i in range(n)],
                             extra_probe_losses_8400=extra_probe_losses,
                             main_revisit_reference=[ref[i]["main_revisit"] for i in range(n)],
                             main_revisit_candidate=[cand[i]["main_revisit"] for i in range(n)],
                             wins_reference=wins_ref, wins_candidate=wins_cand)
    result["functional_pass"] = (
        sum(count > 0 for count in returns) >= 2 and
        sum(extra_probe_losses) <= 0 and wins_cand >= wins_ref)
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
