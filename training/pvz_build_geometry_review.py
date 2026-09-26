"""Source-pinned offline geometry review of accepted PvZ construction leases."""

import argparse
from bisect import bisect_right
from collections import Counter
import json
from math import hypot
from pathlib import Path

from .schema import sha256


def read_log(path):
    samples = {}
    leases = []
    build_actions = []
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip().split(",")
            if fields[0] == "ENTITY" and len(fields) > 7 and fields[2] == "self":
                frame = int(fields[1])
                values = dict(item.split("=", 1) for item in fields[15:] if "=" in item)
                samples.setdefault(frame, {})[int(fields[3])] = dict(
                    type=fields[4], x=int(fields[5]), y=int(fields[6]),
                    hp=int(fields[7]), shields=int(fields[8]),
                    order=values.get("nativeOrder", "unknown"))
            elif fields[0] == "BUILDLEASE":
                lease = dict(item.split("=", 1) for item in fields[2:] if "=" in item)
                lease["frame"] = int(fields[1])
                for key in ("builder", "issued", "targetX", "targetY", "builderX",
                            "builderY", "lastProgress", "commandedBuild",
                            "buildTypeMatches", "builderCanBuildHere", "mapCanBuildHere",
                            "hasPath"):
                    lease[key] = int(lease[key])
                leases.append(lease)
            elif fields[0] == "ACTION" and len(fields) > 7 and fields[4] == "Build" and \
                    fields[6] == "accepted":
                values = dict(item.split("=", 1) for item in fields[7:] if "=" in item)
                build_actions.append(dict(frame=int(fields[1]), builder=int(fields[2]),
                                          kind=int(values.get("extra", -1)),
                                          x=int(values["x"]), y=int(values["y"])))
    return samples, leases, build_actions


def nearest_sample(samples, frames, frame):
    index = bisect_right(frames, frame) - 1
    if index < 0 or frame - frames[index] > 48:
        return None, {}
    return frames[index], samples[frames[index]]


def review(reference_report_path, candidate_report_path):
    reference = json.loads(reference_report_path.read_text())
    candidate = json.loads(candidate_report_path.read_text())
    rows = []
    for group, report in (("reference", reference), ("candidate", candidate)):
        campaign = (reference_report_path.parent / "arena" if group == "reference" else
                    candidate_report_path.parent / "candidate")
        for game in report["rows"]:
            game_id = game["game_id"]
            path = campaign / f"server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
            if sha256(path) != game["log_sha256"]:
                raise ValueError(f"archived log changed: {path}")
            samples, leases, actions = read_log(path)
            frames = sorted(samples)
            for lease in leases:
                if lease["kind"] not in ("Protoss_Pylon", "Protoss_Cybernetics_Core"):
                    continue
                site_x = lease["targetX"] + (32 if lease["kind"] == "Protoss_Pylon" else 48)
                site_y = lease["targetY"] + 32
                sample_frame, units = nearest_sample(samples, frames, lease["issued"])
                probes = [(unit_id, hypot(unit["x"] - site_x, unit["y"] - site_y))
                          for unit_id, unit in units.items() if unit["type"] == "Probe"]
                probes.sort(key=lambda item: item[1])
                builder_distance = next((round(distance) for unit_id, distance in probes
                                         if unit_id == lease["builder"]), None)
                other_distance = next((round(distance) for unit_id, distance in probes
                                       if unit_id != lease["builder"]), None)
                nearest_other = next(((unit_id, round(distance)) for unit_id, distance in probes
                                      if unit_id != lease["builder"]), None)
                _, release_units = nearest_sample(samples, frames, lease["frame"])
                nearby = Counter(unit["type"] for unit in release_units.values()
                                 if hypot(unit["x"] - site_x, unit["y"] - site_y) <= 96)
                matching = [action for action in actions if action["frame"] == lease["issued"] and
                            action["builder"] == lease["builder"] and
                            action["x"] == lease["targetX"] and
                            action["y"] == lease["targetY"]]
                retries = sum(action["builder"] == lease["builder"] and
                              action["x"] == lease["targetX"] and
                              action["y"] == lease["targetY"] and
                              lease["issued"] < action["frame"] <= lease["frame"]
                              for action in actions)
                rows.append(dict(group=group, game_id=game_id, kind=lease["kind"],
                                 issued=lease["issued"], released=lease["frame"],
                                 age=lease["frame"] - lease["issued"],
                                 reason=lease["reason"], builder=lease["builder"],
                                 target=[lease["targetX"], lease["targetY"]],
                                 issue_sample_frame=sample_frame,
                                 builder_issue_distance=builder_distance,
                                 nearest_other_probe_distance=other_distance,
                                 nearest_other_probe_id=(nearest_other[0] if nearest_other else None),
                                 nearest_other_probe_order=(units[nearest_other[0]]["order"]
                                                            if nearest_other else None),
                                 nearest_other_probe_hp=(units[nearest_other[0]]["hp"]
                                                         if nearest_other else None),
                                 nearest_other_probe_shields=(units[nearest_other[0]]["shields"]
                                                              if nearest_other else None),
                                 closer_probe_count=sum(distance < builder_distance for unit_id, distance
                                                        in probes if builder_distance is not None and
                                                        unit_id != lease["builder"]),
                                 builder_release_distance=round(hypot(
                                     lease["builderX"] - site_x, lease["builderY"] - site_y)),
                                 progress_age=lease["frame"] - lease["lastProgress"],
                                 native_order=lease["order"],
                                 worker_buildable=bool(lease["builderCanBuildHere"]),
                                 map_buildable=bool(lease["mapCanBuildHere"]),
                                 has_path=bool(lease["hasPath"]),
                                 nearby_self_entities=dict(nearby),
                                 exact_issue_action_logged=bool(matching),
                                 repeated_accepted_build_commands=retries))
    pylons = [row for row in rows if row["kind"] == "Protoss_Pylon" and
              row["reason"] == "hard-lease-limit"]
    return dict(schema="protodd-pvz-build-geometry-review-v1",
                source_sha256=sha256(__file__),
                reference_report_sha256=sha256(reference_report_path),
                candidate_report_sha256=sha256(candidate_report_path), rows=rows,
                summary=dict(pylon_hard_releases=len(pylons),
                             with_closer_visible_probe=sum(row["closer_probe_count"] > 0
                                                           for row in pylons),
                             near_site_at_release=sum(row["builder_release_distance"] <= 128
                                                      for row in pylons),
                             buildable_and_path=sum(row["worker_buildable"] and row["has_path"]
                                                    for row in pylons)),
                strength_validated=False, promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference_report", type=Path)
    parser.add_argument("candidate_report", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.reference_report, args.candidate_report)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result["summary"]))
