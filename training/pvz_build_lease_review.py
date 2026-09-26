"""Audit accepted Core build orders and builder releases in frozen PvZ games."""

import argparse
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256


def read_log(path):
    match = None
    first_issue = None
    first_create = None
    first_complete = None
    builds = []
    releases = []
    active_core_builds = {}
    placement_failures = []
    final_frame = None
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if fields[0] == "MATCH":
                match = dict(item.split("=", 1) for item in fields[1:] if "=" in item)
            elif fields[0] == "MACRO" and len(fields) > 5 and fields[3] == "Cybernetics Core":
                frame = int(fields[1])
                if first_issue is None and fields[5].startswith("issued-"):
                    first_issue = frame
                if "placement-search" in fields[5]:
                    placement_failures.append(frame)
            elif fields[0] == "LIFECYCLE" and len(fields) > 5 and fields[3] == "self" and \
                    fields[5] == "Protoss_Cybernetics_Core":
                frame = int(fields[1])
                if fields[2] == "create" and first_create is None:
                    first_create = frame
                if fields[2] == "complete" and first_complete is None:
                    first_complete = frame
            elif fields[0] == "ACTION" and len(fields) > 6:
                frame = int(fields[1])
                builder = int(fields[2])
                if fields[4] == "Build" and fields[6] == "accepted":
                    active_core_builds.pop(builder, None)
                if fields[4] == "Build" and "extra=164" in line and fields[6] == "accepted":
                    values = dict(item.split("=", 1) for item in fields[7:] if "=" in item)
                    build = dict(frame=frame, builder=builder,
                                 x=int(values["x"]), y=int(values["y"]))
                    builds.append(build)
                    active_core_builds[builder] = build
                elif fields[3] == "builder-release" and fields[4] == "Stop" and \
                        fields[6] == "accepted":
                    build = active_core_builds.pop(builder, None)
                    if build is not None:
                        releases.append(dict(frame=frame, builder=builder,
                                             build_frame=build["frame"],
                                             x=build["x"], y=build["y"]))
            elif fields[0] == "SUMMARY":
                values = dict(item.split("=", 1) for item in fields[1:] if "=" in item)
                final_frame = int(values["frames"])
    core_releases = [row for row in releases if first_issue is not None and
                     row["frame"] >= first_issue and
                     (first_create is None or row["frame"] <= first_create)]
    return dict(match=match, first_issue=first_issue, first_create=first_create,
                first_complete=first_complete,
                issue_to_create=(None if first_issue is None or first_create is None else
                                 first_create - first_issue),
                accepted_core_builds=[row for row in builds if first_issue is None or
                                      row["frame"] >= first_issue and
                                      (first_create is None or row["frame"] <= first_create)],
                core_builder_releases=core_releases,
                core_placement_failures=[frame for frame in placement_failures if
                                         first_issue is not None and frame >= first_issue and
                                         (first_create is None or frame <= first_create)],
                final_frame=final_frame, log_sha256=sha256(path))


def review(paths):
    rows = []
    manifests = []
    for campaign_path in paths:
        campaign_path = Path(campaign_path).resolve()
        verify(campaign_path)
        status = inspect(campaign_path)
        manifest = json.loads((campaign_path / "manifest.json").read_text())
        if status["excluded"] or len(status["structurally_valid"]) != manifest["games"]:
            raise ValueError(f"unhealthy campaign: {campaign_path}")
        manifests.append(dict(path=str(campaign_path), sha256=sha256(campaign_path / "manifest.json"),
                              dll_sha256=manifest["components"]["server/bots/Protodd/AI/Protodd.dll"]))
        for game in status["structurally_valid"]:
            game_id = game["game_id"]
            log = campaign_path / f"server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
            rows.append(dict(campaign=campaign_path.parent.name + "/" + campaign_path.name,
                             game_id=game_id, won=game["won"],
                             reported_frame=game["frame"], **read_log(log)))
    if len({item["dll_sha256"] for item in manifests}) != 1:
        raise ValueError("campaigns use different DLLs")
    delays = sorted(row["issue_to_create"] for row in rows if row["issue_to_create"] is not None)
    return dict(schema="protodd-pvz-build-lease-review-v1",
                review_source_sha256=sha256(__file__), manifests=manifests, rows=rows,
                summary=dict(games=len(rows), comparable_core_builds=len(delays),
                             median_issue_to_create=delays[len(delays) // 2] if delays else None,
                             delays_over_480=sum(delay > 480 for delay in delays),
                             issued_without_creation=sum(row["first_issue"] is not None and
                                                         row["first_create"] is None for row in rows)),
                promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--campaign", action="append", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.campaign)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"]))
