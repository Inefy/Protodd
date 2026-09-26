"""Review observation-only build lease telemetry from a bounded PvZ arena run."""

import argparse
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256


def review(plan_path):
    plan = json.loads(plan_path.read_text())
    if sha256(__file__) != plan["reviewer_sha256"]:
        raise ValueError("reviewer source changed")
    root = plan_path.parent
    for relative, digest in plan["sha256"].items():
        path = root / relative
        if sha256(path) != digest:
            raise ValueError(f"source/input changed: {path}")
    arena = root / "arena"
    verify(arena)
    health = inspect(arena)
    if health["excluded"] or len(health["structurally_valid"]) != plan["games"]:
        raise ValueError("arena has missing or unhealthy games")
    rows = []
    for game in health["structurally_valid"]:
        log = arena / f"server/replays/bot-write/game-{game['game_id']}/Protodd/received/Protodd.log"
        leases = []
        core_issued = []
        core_created = []
        with log.open(errors="replace") as stream:
            for line in stream:
                fields = line.rstrip().split(",")
                if fields[0] == "BUILDLEASE":
                    row = dict(item.split("=", 1) for item in fields[2:] if "=" in item)
                    row["frame"] = int(fields[1])
                    for key in ("builder", "issued", "targetX", "targetY", "builderX",
                                "builderY", "lastProgress", "commandedBuild",
                                "buildTypeMatches", "builderCanBuildHere", "mapCanBuildHere",
                                "hasPath"):
                        row[key] = int(row[key])
                    leases.append(row)
                elif fields[0] == "ACTION" and len(fields) > 6 and fields[4] == "Build" and \
                        fields[6] == "accepted" and "extra=164" in line:
                    core_issued.append(int(fields[1]))
                elif fields[0] == "LIFECYCLE" and len(fields) > 5 and fields[2] == "create" and \
                        fields[3] == "self" and fields[5] == "Protoss_Cybernetics_Core":
                    core_created.append(int(fields[1]))
        rows.append(dict(game_id=game["game_id"], frame=game["frame"], won=game["won"],
                         core_issued=core_issued, core_created=core_created,
                         leases=leases, log_sha256=sha256(log)))
    return dict(schema="protodd-pvz-build-lease-diagnostic-v1", plan_sha256=sha256(plan_path),
                manifest_sha256=sha256(arena / "manifest.json"), rows=rows,
                summary=dict(games=len(rows), core_issues=sum(len(row["core_issued"]) for row in rows),
                             core_creations=sum(len(row["core_created"]) for row in rows),
                             lease_ends=sum(len(row["leases"]) for row in rows),
                             core_lease_ends=sum(sum(lease["kind"] == "Protoss_Cybernetics_Core"
                                                     for lease in row["leases"]) for row in rows)),
                promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"]))
