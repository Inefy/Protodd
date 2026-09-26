"""Review frame-capped build diagnostics without treating time-limit results as wins."""

import argparse
from collections import Counter, defaultdict
import json
from pathlib import Path

from .arena import verify
from .schema import sha256


def review(plan_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    for relative, digest in plan["sha256"].items():
        if sha256(root / relative) != digest:
            raise ValueError(f"frozen source/input changed: {relative}")
    arena = root / plan.get("arena_directory", "arena")
    verify(arena)
    schedule = {row["gameID"]: row for row in map(json.loads,
                (arena / "server/games.jsonl").read_text().splitlines())}
    grouped = defaultdict(list)
    for line in (arena / "server/results.jsonl").read_text().splitlines():
        result = json.loads(line)
        grouped[result["gameID"]].append(result)
    if set(grouped) != set(schedule) or len(schedule) != plan["games"]:
        raise ValueError("missing or extra game reports")
    rows = []
    for game_id, game in sorted(schedule.items()):
        pair = grouped[game_id]
        if len(pair) != 2 or {r["reportingBot"] for r in pair} != \
                {game["homeBot"], game["awayBot"]}:
            raise ValueError(f"missing pair: {game_id}")
        for result in pair:
            if result["opponentBot"] not in (game["homeBot"], game["awayBot"]) or \
                    result["opponentBot"] == result["reportingBot"] or \
                    result["map"] != game["map"] or \
                    result["wasHost"] != (result["reportingBot"] == game["homeBot"]) or \
                    result["gameEndType"] != "NORMAL" or result["crash"] or \
                    result["gameTimeout"] or result["won"] or \
                    result["finalFrame"] != plan["frame_limit"] + 2:
                raise ValueError(f"unhealthy or unexpected frame-capped pair: {game_id}")
        if len({r["finalFrame"] for r in pair}) != 1:
            raise ValueError(f"frame mismatch: {game_id}")
        log = arena / f"server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
        match = None
        leases = []
        core_issued = []
        core_created = []
        pylon_created = []
        with log.open(errors="replace") as stream:
            for line in stream:
                fields = line.rstrip().split(",")
                if fields[0] == "MATCH":
                    match = dict(item.split("=", 1) for item in fields[1:] if "=" in item)
                elif fields[0] == "BUILDLEASE":
                    lease = dict(item.split("=", 1) for item in fields[2:] if "=" in item)
                    lease["frame"] = int(fields[1])
                    for key in ("builder", "issued", "targetX", "targetY", "builderX",
                                "builderY", "lastProgress", "commandedBuild",
                                "buildTypeMatches", "builderCanBuildHere", "mapCanBuildHere",
                                "hasPath"):
                        lease[key] = int(lease[key])
                    leases.append(lease)
                elif fields[0] == "ACTION" and len(fields) > 6 and fields[4] == "Build" and \
                        fields[6] == "accepted" and "extra=164" in line:
                    core_issued.append(int(fields[1]))
                elif fields[0] == "LIFECYCLE" and len(fields) > 5 and fields[2] == "create" and \
                        fields[3] == "self":
                    if fields[5] == "Protoss_Cybernetics_Core":
                        core_created.append(int(fields[1]))
                    elif fields[5] == "Protoss_Pylon":
                        pylon_created.append(int(fields[1]))
        if match is None:
            raise ValueError(f"no match identity in game {game_id}")
        rows.append(dict(game_id=game_id, map=game["map"], home=game["homeBot"],
                         seed=match["seed"], map_hash=match["map_hash"],
                         frame=pair[0]["finalFrame"], core_issued=core_issued,
                         core_created=core_created, pylon_created=pylon_created,
                         leases=leases, log_sha256=sha256(log)))
    all_leases = [lease for row in rows for lease in row["leases"]]
    return dict(schema="protodd-pvz-build-lease-capped-review-v1",
                plan_sha256=sha256(plan_path), review_source_sha256=sha256(__file__),
                manifest_sha256=sha256(arena / "manifest.json"), rows=rows,
                summary=dict(games=len(rows), frame_capped=True,
                             win_labels_valid=False,
                             core_issues=sum(len(row["core_issued"]) for row in rows),
                             core_creations=sum(len(row["core_created"]) for row in rows),
                             lease_ends=len(all_leases),
                             lease_kinds=dict(Counter(lease["kind"] for lease in all_leases)),
                             lease_reasons=dict(Counter(lease["reason"] for lease in all_leases)),
                             moving_pylon_hard_releases=sum(
                                 lease["kind"] == "Protoss_Pylon" and
                                 lease["reason"] == "hard-lease-limit" and
                                 lease["order"] == "PlaceBuilding" and
                                 lease["buildTypeMatches"] == 1 and
                                 lease["frame"] - lease["lastProgress"] <= 16
                                 for lease in all_leases)),
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
