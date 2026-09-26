"""Apply frozen functional gates to the site-builder PvZ development candidate."""

import argparse
import json
from pathlib import Path

from .schema import sha256


def screen(plan_path, candidate_report_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    candidate = json.loads(candidate_report_path.read_text())
    reference_path = root.parent / "pvz-builder-select-20260925/diagnostic-report.json"
    if sha256(reference_path) != plan["reference_report_sha256"]:
        raise ValueError("reference report changed")
    reference = json.loads(reference_path.read_text())
    if candidate["plan_sha256"] != sha256(plan_path) or \
            candidate["review_source_sha256"] != plan["reviewer_sha256"]:
        raise ValueError("candidate reviewer or plan differs")
    reference_components = json.loads((root.parent / "pvz-builder-select-20260925" /
                                       "arena/manifest.json").read_text())["components"]
    candidate_components = json.loads((root / "candidate/manifest.json").read_text())["components"]
    differing = {name for name in reference_components if
                 candidate_components.get(name) != reference_components[name]}
    if set(reference_components) != set(candidate_components) or \
            differing != {"server/bots/Protodd/AI/Protodd.dll"}:
        raise ValueError(f"prepared inputs differ beyond DLL: {differing}")
    reference_rows = {row["game_id"]: row for row in reference["rows"]}
    candidate_rows = {row["game_id"]: row for row in candidate["rows"]}
    if set(reference_rows) != set(candidate_rows) or len(candidate_rows) != plan["games"]:
        raise ValueError("missing or extra game")
    paired = []
    handoffs = []
    for game_id in sorted(candidate_rows):
        a, b = reference_rows[game_id], candidate_rows[game_id]
        if (a["seed"], a["map_hash"], a["map"], a["home"]) != \
                (b["seed"], b["map_hash"], b["map"], b["home"]):
            raise ValueError(f"seed/map/host mismatch: {game_id}")
        paired.append(dict(game_id=game_id, seed=a["seed"], map=a["map"],
                           home=a["home"], reference_frame=a["frame"],
                           candidate_frame=b["frame"], candidate_capped=b["capped"],
                           reference_pylons=len(a["pylon_created"]),
                           candidate_pylons=len(b["pylon_created"])))
        log = root / f"candidate/server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
        if sha256(log) != b["log_sha256"]:
            raise ValueError(f"candidate log changed: {log}")
        selected = []
        accepted = []
        with log.open(errors="replace") as stream:
            for line in stream:
                fields = line.rstrip().split(",")
                if fields[0] == "BUILDSELECT":
                    item = dict(part.split("=", 1) for part in fields[2:] if "=" in part)
                    item["frame"] = int(fields[1])
                    selected.append(item)
                elif fields[0] == "ACTION" and len(fields) > 7 and fields[4] == "Build" and \
                        fields[6] == "accepted":
                    item = dict(part.split("=", 1) for part in fields[7:] if "=" in part)
                    accepted.append(dict(frame=int(fields[1]), actor=int(fields[2]),
                                         kind=item.get("extra"), x=item.get("x"), y=item.get("y")))
        for item in selected:
            if item["kind"] != "Protoss_Pylon" or item["selected"] == item["siteCandidate"]:
                continue
            if int(item["selectedDistance"]) - int(item["candidateDistance"]) < 256 or \
                    item["candidateCanBuildHere"] != "1" or item["candidateHasPath"] != "1":
                continue
            matching = [action for action in accepted if action["frame"] == item["frame"] and
                        action["kind"] == "156" and action["x"] == item["targetX"] and
                        action["y"] == item["targetY"]]
            if any(action["actor"] == int(item["siteCandidate"]) for action in matching):
                handoffs.append(dict(game_id=game_id, frame=item["frame"],
                                     old_builder=int(item["selected"]),
                                     new_builder=int(item["siteCandidate"]),
                                     old_distance=int(item["selectedDistance"]),
                                     new_distance=int(item["candidateDistance"])))
    reference_hard = sum(lease["kind"] == "Protoss_Pylon" and
                         lease["reason"] == "hard-lease-limit"
                         for row in reference["rows"] for lease in row["leases"])
    candidate_hard = sum(lease["kind"] == "Protoss_Pylon" and
                         lease["reason"] == "hard-lease-limit"
                         for row in candidate["rows"] for lease in row["leases"])
    pylons = sum(len(row["pylon_created"]) for row in candidate["rows"])
    cores = sum(len(row["core_created"]) for row in candidate["rows"])
    early_losses = sum(row["decisive_won"] is False for row in candidate["rows"])
    gates = plan["functional_gate"]
    passed = (len(paired) == gates["normal_games"] and
              len(handoffs) >= gates["minimum_accepted_handoffs"] and
              candidate_hard <= gates["maximum_pylon_hard_releases"] and
              pylons >= gates["minimum_pylons_created"] and
              cores >= gates["minimum_core_creations"] and
              early_losses <= gates["maximum_early_decisive_losses"])
    return dict(schema="protodd-pvz-builder-site-screen-v1", plan_sha256=sha256(plan_path),
                source_sha256=sha256(__file__), reference_report_sha256=sha256(reference_path),
                candidate_report_sha256=sha256(candidate_report_path),
                paired=paired, handoffs=handoffs, reference_hard_releases=reference_hard,
                candidate_hard_releases=candidate_hard, candidate_pylons=pylons,
                candidate_cores=cores, early_losses=early_losses, gates=gates,
                passed=passed, strength_validated=False, promotion_allowed=False)


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
    print(json.dumps({name: result[name] for name in (
        "passed", "candidate_hard_releases", "candidate_pylons", "candidate_cores",
        "early_losses")} | {"accepted_handoffs": len(result["handoffs"])}))
