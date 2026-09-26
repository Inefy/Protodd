"""Apply frozen functional gates to source-pinned frame-capped Pylon lease games."""

import argparse
import json
from pathlib import Path

from .schema import sha256


def screen(plan_path, candidate_report_path):
    plan = json.loads(plan_path.read_text())
    root = plan_path.parent
    reference_path = root.parent / "pvz-build-lease-diagnostic-20260925" / "diagnostic-report-capped.json"
    if sha256(reference_path) != plan["reference_report_sha256"]:
        raise ValueError("reference report changed")
    reference = json.loads(reference_path.read_text())
    candidate = json.loads(candidate_report_path.read_text())
    if candidate["plan_sha256"] != sha256(plan_path):
        raise ValueError("candidate report has different plan")
    if candidate["review_source_sha256"] != plan["reviewer_sha256"]:
        raise ValueError("candidate reviewer differs")
    reference_manifest = json.loads((root.parent / "pvz-build-lease-diagnostic-20260925" /
                                     "arena/manifest.json").read_text())["components"]
    candidate_manifest = json.loads((root / "candidate/manifest.json").read_text())["components"]
    difference = {name for name in reference_manifest if
                  candidate_manifest.get(name) != reference_manifest[name]}
    if difference != {"server/bots/Protodd/AI/Protodd.dll"} or \
            set(reference_manifest) != set(candidate_manifest):
        raise ValueError(f"prepared inputs differ beyond DLL: {difference}")
    original = {row["game_id"]: row for row in reference["rows"]}
    treatment = {row["game_id"]: row for row in candidate["rows"]}
    if set(original) != set(treatment) or len(original) != plan["games"]:
        raise ValueError("different game schedules")
    paired = []
    for game_id in sorted(original):
        a, b = original[game_id], treatment[game_id]
        if (a["seed"], a["map_hash"], a["map"], a["home"], a["frame"]) != \
                (b["seed"], b["map_hash"], b["map"], b["home"], b["frame"]):
            raise ValueError(f"seed, map, host or frame mismatch: {game_id}")
        paired.append(dict(game_id=game_id, seed=a["seed"], map=a["map"], home=a["home"],
                           reference_pylons=len(a["pylon_created"]),
                           candidate_pylons=len(b["pylon_created"]),
                           reference_cores=len(a["core_created"]),
                           candidate_cores=len(b["core_created"])))
    gates = plan["functional_gate"]
    moving_hard = candidate["summary"]["moving_pylon_hard_releases"]
    pylons = sum(len(row["pylon_created"]) for row in candidate["rows"])
    cores = sum(len(row["core_created"]) for row in candidate["rows"])
    passed = (len(paired) == gates["normal_capped_pairs"] and
              moving_hard <= gates["max_moving_pylon_hard_releases"] and
              pylons >= gates["minimum_pylons_created"] and
              cores >= gates["minimum_core_creations"])
    return dict(schema="protodd-pvz-pylon-lease-screen-v1", plan_sha256=sha256(plan_path),
                source_sha256=sha256(__file__), reference_report_sha256=sha256(reference_path),
                candidate_report_sha256=sha256(candidate_report_path), paired=paired,
                reference_summary=reference["summary"], candidate_summary=candidate["summary"],
                gates=gates, passed=passed, strength_validated=False,
                promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("candidate_report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = screen(args.plan, args.candidate_report)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(dict(passed=result["passed"], paired=result["paired"])))
