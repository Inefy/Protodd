"""Audit site-anchored eligible builders in a frozen observation-only arena."""

import argparse
from collections import Counter
import json
from pathlib import Path

from .schema import sha256


def review(plan_path, diagnostic_report_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    diagnostic = json.loads(diagnostic_report_path.read_text())
    if diagnostic["plan_sha256"] != sha256(plan_path):
        raise ValueError("diagnostic report does not bind to plan")
    for relative, digest in plan["sha256"].items():
        if sha256(root / relative) != digest:
            raise ValueError(f"frozen source/input changed: {relative}")
    rows = []
    for game in diagnostic["rows"]:
        path = root / f"arena/server/replays/bot-write/game-{game['game_id']}/Protodd/received/Protodd.log"
        if sha256(path) != game["log_sha256"]:
            raise ValueError(f"log changed: {path}")
        selections = []
        leases = []
        actions = []
        with path.open(errors="replace") as stream:
            for line in stream:
                fields = line.rstrip().split(",")
                if fields[0] == "BUILDSELECT":
                    item = dict(part.split("=", 1) for part in fields[2:] if "=" in part)
                    item["frame"] = int(fields[1])
                    for key in ("selected", "siteCandidate", "targetX", "targetY",
                                "anchorX", "anchorY", "selectedDistance",
                                "candidateDistance", "candidateCanBuildHere", "candidateHasPath"):
                        item[key] = int(item[key])
                    selections.append(item)
                elif fields[0] == "BUILDLEASE":
                    item = dict(part.split("=", 1) for part in fields[2:] if "=" in part)
                    item["frame"] = int(fields[1])
                    for key in ("builder", "issued", "targetX", "targetY"):
                        item[key] = int(item[key])
                    leases.append(item)
                elif fields[0] == "ACTION" and len(fields) > 7 and fields[4] == "Build" and \
                        fields[6] == "accepted":
                    values = dict(part.split("=", 1) for part in fields[7:] if "=" in part)
                    if values.get("extra") in ("156", "164"):
                        actions.append(dict(frame=int(fields[1]), actor=int(fields[2]),
                                            kind=int(values["extra"]),
                                            x=int(values["x"]), y=int(values["y"])))
        for selection in selections:
            accepted = any(a["frame"] == selection["frame"] and
                           a["actor"] == selection["selected"] and
                           a["x"] == selection["targetX"] and
                           a["y"] == selection["targetY"]
                           for a in actions)
            matching_lease = next((lease for lease in leases
                                   if lease["issued"] == selection["frame"] and
                                   lease["builder"] == selection["selected"] and
                                   lease["targetX"] == selection["targetX"] and
                                   lease["targetY"] == selection["targetY"]), None)
            better = (selection["siteCandidate"] >= 0 and
                      selection["siteCandidate"] != selection["selected"] and
                      selection["candidateCanBuildHere"] == 1 and
                      selection["candidateHasPath"] == 1 and
                      selection["selectedDistance"] - selection["candidateDistance"] >= 256)
            rows.append(dict(game_id=game["game_id"], seed=game["seed"],
                             kind=selection["kind"], frame=selection["frame"],
                             selected=selection["selected"],
                             site_candidate=selection["siteCandidate"],
                             selected_distance=selection["selectedDistance"],
                             candidate_distance=selection["candidateDistance"],
                             candidate_can_build=bool(selection["candidateCanBuildHere"]),
                             candidate_has_path=bool(selection["candidateHasPath"]),
                             target=[selection["targetX"], selection["targetY"]],
                             anchor=[selection["anchorX"], selection["anchorY"]],
                             accepted_action_logged=accepted,
                             lease_reason=matching_lease["reason"] if matching_lease else None,
                             site_eligible_better=better))
    return dict(schema="protodd-pvz-build-selection-review-v1",
                plan_sha256=sha256(plan_path), source_sha256=sha256(__file__),
                diagnostic_report_sha256=sha256(diagnostic_report_path), rows=rows,
                summary=dict(selections=len(rows),
                             kinds=dict(Counter(row["kind"] for row in rows)),
                             site_eligible_better=sum(row["site_eligible_better"] for row in rows),
                             accepted_pylon_better=sum(row["site_eligible_better"] and
                                                       row["kind"] == "Protoss_Pylon" and
                                                       row["accepted_action_logged"] for row in rows),
                             failed_pylon_better=sum(row["site_eligible_better"] and
                                                     row["kind"] == "Protoss_Pylon" and
                                                     row["lease_reason"] is not None for row in rows)),
                strength_validated=False, promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("diagnostic_report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.diagnostic_report)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"]))
