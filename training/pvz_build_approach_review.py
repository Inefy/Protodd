"""Read-only, source-pinned review of the final approach of Pylon builders."""

import argparse
from collections import Counter
import json
from math import hypot
from pathlib import Path

from .pvz_build_geometry_review import read_log
from .schema import sha256


def summarize_track(track, release_frame, release_distance):
    if not track:
        return {"samples": 0}
    distances = [point["distance"] for point in track]
    last = track[-1]
    recent = [point for point in track if release_frame - point["frame"] <= 96]
    return {
        "samples": len(track),
        "first_frame": track[0]["frame"],
        "last_frame": last["frame"],
        "first_distance": distances[0],
        "minimum_sampled_distance": min(distances),
        "last_sampled_distance": distances[-1],
        "release_distance": release_distance,
        "entered_96": min(distances) <= 96,
        "entered_128": min(distances) <= 128,
        "last_96_frame_progress": (recent[0]["distance"] - recent[-1]["distance"]
                                   if len(recent) >= 2 else None),
        "last_96_net_movement": (round(hypot(recent[0]["x"] - recent[-1]["x"],
                                               recent[0]["y"] - recent[-1]["y"]))
                                 if len(recent) >= 2 else None),
        "recent_minimum_distance": min(p["distance"] for p in recent),
        "recent_orders": dict(Counter(p["order"] for p in recent)),
        "track": track,
    }


def review(reference_report_path, candidate_report_path):
    rows = []
    for group, report_path, arena_name in (
        ("reference", reference_report_path, "arena"),
        ("candidate", candidate_report_path, "candidate"),
    ):
        report = json.loads(report_path.read_text())
        for game in report["rows"]:
            game_id = game["game_id"]
            log_path = (report_path.parent / arena_name / "server/replays/bot-write" /
                        f"game-{game_id}/Protodd/received/Protodd.log")
            log_hash = sha256(log_path)
            if log_hash != game["log_sha256"]:
                raise ValueError(f"archived log changed: {log_path}")
            samples, leases, _ = read_log(log_path)
            for lease in leases:
                if lease["kind"] != "Protoss_Pylon" or lease["reason"] != "hard-lease-limit":
                    continue
                site_x, site_y = lease["targetX"] + 32, lease["targetY"] + 32
                track = []
                for frame in sorted(samples):
                    if not lease["issued"] <= frame <= lease["frame"]:
                        continue
                    builder = samples[frame].get(lease["builder"])
                    if builder is None:
                        continue
                    track.append({"frame": frame, "x": builder["x"], "y": builder["y"],
                                  "distance": round(hypot(builder["x"] - site_x,
                                                          builder["y"] - site_y)),
                                  "order": builder["order"]})
                release_distance = round(hypot(lease["builderX"] - site_x,
                                               lease["builderY"] - site_y))
                rows.append({"group": group, "game_id": game_id, "log_sha256": log_hash,
                             "builder": lease["builder"], "issued": lease["issued"],
                             "released": lease["frame"], "target": [lease["targetX"],
                                                                   lease["targetY"]],
                             "release_order": lease["order"],
                             "release_progress_age": lease["frame"] - lease["lastProgress"],
                             "worker_buildable": bool(lease["builderCanBuildHere"]),
                             "map_buildable": bool(lease["mapCanBuildHere"]),
                             "has_path": bool(lease["hasPath"]),
                             **summarize_track(track, lease["frame"], release_distance)})
    near = [r for r in rows if r["group"] == "candidate" and 96 < r["release_distance"] <= 128]
    return {
        "schema": "protodd-pvz-build-approach-review-v1",
        "source_sha256": sha256(__file__),
        "reference_report_sha256": sha256(reference_report_path),
        "candidate_report_sha256": sha256(candidate_report_path),
        "reference_bridge_sha256": sha256(reference_report_path.parent / "source/BwapiBridge.cpp"),
        "candidate_bridge_sha256": sha256(candidate_report_path.parent / "source/BwapiBridge.cpp"),
        "summary": {
            "reference_pylon_hard_releases": sum(r["group"] == "reference" for r in rows),
            "candidate_pylon_hard_releases": sum(r["group"] == "candidate" for r in rows),
            "candidate_release_97_to_128": len(near),
            "candidate_release_124_to_127": sum(124 <= r["release_distance"] <= 127 for r in near),
            "near_orders_still_place_building": sum(r["release_order"] == "PlaceBuilding" for r in near),
            "near_orders_progress_within_4_frames": sum(r["release_progress_age"] <= 4 for r in near),
            "near_orders_worker_buildable_with_path": sum(r["worker_buildable"] and r["has_path"] for r in near),
            "near_orders_ever_sampled_within_96": sum(r["entered_96"] for r in near),
            "near_orders_recently_closing_16px": sum((r["last_96_frame_progress"] or 0) >= 16 for r in near),
            "near_orders_recently_moving_16px": sum((r["last_96_net_movement"] or 0) >= 16 for r in near),
        },
        "rows": rows,
        "strength_validated": False,
        "promotion_allowed": False,
    }


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
