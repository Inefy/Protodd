"""Frozen functional screen for the bounded Pylon final-approach grace."""

import argparse
import json
from pathlib import Path

from .schema import sha256


def delayed_pylons(log_path):
    actions = []
    creations = []
    with log_path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip().split(",")
            if fields[0] == "ACTION" and len(fields) > 7 and fields[4] == "Build" and \
                    fields[6] == "accepted" and "extra=156" in line:
                values = dict(item.split("=", 1) for item in fields[7:] if "=" in item)
                actions.append((int(fields[1]), int(values["x"]), int(values["y"])))
            elif fields[0] == "LIFECYCLE" and len(fields) > 6 and \
                    fields[2:4] == ["create", "self"] and fields[5] == "Protoss_Pylon":
                values = dict(item.split("=", 1) for item in fields[6:] if "=" in item)
                creations.append((int(fields[1]), int(values["x"]) - 32,
                                  int(values["y"]) - 32))
    matched = []
    for created, x, y in creations:
        issued = [frame for frame, ax, ay in actions if frame <= created and ax == x and ay == y]
        if issued:
            lag = created - max(issued)
            if lag >= 8 * 24:
                matched.append({"issued": max(issued), "created": created,
                                "target": [x, y], "lag": lag})
    return matched


def screen(plan_path, report_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    candidate = json.loads(report_path.read_text())
    reference_path = root.parent / "pvz-build-lease-diagnostic-20260925/diagnostic-report-capped.json"
    if sha256(reference_path) != plan["reference_report_sha256"]:
        raise ValueError("reference report changed")
    reference = json.loads(reference_path.read_text())
    if candidate["plan_sha256"] != sha256(plan_path) or \
            candidate["review_source_sha256"] != plan["reviewer_sha256"]:
        raise ValueError("candidate reviewer or plan differs")
    reference_components = json.loads((root.parent / "pvz-build-lease-diagnostic-20260925" /
                                       "arena/manifest.json").read_text())["components"]
    candidate_components = json.loads((root / plan["arena_directory"] /
                                       "manifest.json").read_text())["components"]
    differing = {name for name in reference_components if
                 candidate_components.get(name) != reference_components[name]}
    if set(reference_components) != set(candidate_components) or \
            differing != {"server/bots/Protodd/AI/Protodd.dll"}:
        raise ValueError(f"prepared inputs differ beyond DLL: {differing}")
    ref_rows = {row["game_id"]: row for row in reference["rows"]}
    cand_rows = {row["game_id"]: row for row in candidate["rows"]}
    if set(ref_rows) != set(cand_rows) or len(cand_rows) != plan["games"]:
        raise ValueError("missing or extra game")
    paired = []
    delayed = []
    for game_id in sorted(cand_rows):
        a, b = ref_rows[game_id], cand_rows[game_id]
        if (a["seed"], a["map_hash"], a["map"], a["home"]) != \
                (b["seed"], b["map_hash"], b["map"], b["home"]):
            raise ValueError(f"seed/map/host mismatch: {game_id}")
        paired.append({"game_id": game_id, "seed": b["seed"], "map": b["map"],
                       "home": b["home"], "frame": b["frame"], "capped": b["capped"]})
        log = root / plan["arena_directory"] / \
            f"server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
        if sha256(log) != b["log_sha256"]:
            raise ValueError(f"candidate log changed: {log}")
        delayed.extend({"game_id": game_id, **item} for item in delayed_pylons(log))
    pylons = sum(len(row["pylon_created"]) for row in cand_rows.values())
    cores = sum(len(row["core_created"]) for row in cand_rows.values())
    moving_hard = candidate["summary"]["moving_pylon_hard_releases"]
    gates = plan["functional_gate"]
    passed = (len(paired) == gates["normal_pairs"] and
              sum(row["capped"] for row in paired) >= gates["minimum_capped_pairs"] and
              len(delayed) >= gates["minimum_delayed_pylons_created"] and
              moving_hard <= gates["maximum_moving_pylon_hard_releases"] and
              pylons >= gates["minimum_pylons_created"] and
              cores >= gates["minimum_core_creations"])
    return {"schema": "protodd-pvz-approach-grace-screen-v1",
            "source_sha256": sha256(__file__), "plan_sha256": sha256(plan_path),
            "reference_report_sha256": sha256(reference_path),
            "candidate_report_sha256": sha256(report_path),
            "paired": paired, "delayed_pylons_created": delayed,
            "pylons_created": pylons, "cores_created": cores,
            "moving_pylon_hard_releases": moving_hard, "gates": gates,
            "passed": passed, "strength_validated": False, "promotion_allowed": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = screen(args.plan, args.report)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"passed": result["passed"],
                      "late_pylons": len(result["delayed_pylons_created"]),
                      "moving_hard": result["moving_pylon_hard_releases"],
                      "pylons": result["pylons_created"],
                      "cores": result["cores_created"]}))
