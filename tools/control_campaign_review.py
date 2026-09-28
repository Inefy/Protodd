"""Summarize observed control bottlenecks in a completed local arena campaign.

Counts are logged decision samples, not seconds or independent combat events.
Each frame is counted once per metric even if several squads were logged.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
import json
import re
from collections import Counter, defaultdict
from pathlib import Path


def fields(line: str) -> tuple[str, int, dict[str, str]] | None:
    if line.partition(",")[0] not in {
            "STRATEGY", "SQUAD", "STATE", "DETECTOR_ALLOC"}:
        return None
    parts = line.rstrip("\n").split(",")
    if len(parts) < 3:
        return None
    try:
        frame = int(parts[1])
    except ValueError:
        return None
    values = {}
    for part in parts[2:]:
        if "=" in part:
            key, value = part.split("=", 1)
            values[key] = value
    return parts[0], frame, values


def audit_log(path: Path) -> dict:
    pressure_held = set()
    pressure_released = set()
    forward_third_screen = set()
    expansion_cover_routes = set()
    nexus_samples = []
    main_detection_blocked = set()
    blocked_main_groups = Counter()
    large_main_rally = set()
    large_main_routes = set()
    escort_orders = defaultdict(set)
    detector_wait_volleys = set()
    detector_wait_retreats = set()
    reserve_telemetry_seen = False
    contested_reserve_releases = set()
    peak_army = 0
    peak_probes = 0
    first_three_nexus_frame = None
    nexus_count_drop_with_army = []
    previous_nexuses = None
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("EVENT,"):
                event = line.rstrip("\n").split(",", 3)
                if len(event) > 2 and event[2] == "forward-third-screen":
                    forward_third_screen.add(int(event[1]))
                continue
            if line.startswith("ORDER,"):
                order = line.rstrip("\n").split(",", 8)
                if len(order) > 7:
                    if order[7] == "detector-escort":
                        escort_orders[int(order[1])].add(order[2])
                    elif order[7] == "detector-wait-volley":
                        detector_wait_volleys.add(int(order[1]))
                    elif order[7] == "wait-for-mobile-detection":
                        detector_wait_retreats.add(int(order[1]))
                continue
            parsed = fields(line)
            if parsed is None:
                continue
            kind, frame, values = parsed
            if kind == "STRATEGY":
                if (values.get("strategyPosture") == "Pressure" and
                        values.get("policyEnabled") == "1" and
                        values.get("policyAction") == "3" and
                        values.get("policyWeights") == "0" and
                        line.split(",", 5)[3] == "Hold"):
                    pressure_held.add(frame)
                if values.get("coveredPressureRelease") == "1":
                    pressure_released.add(frame)
            elif kind == "DETECTOR_ALLOC":
                reserve_telemetry_seen = True
                observers = int(values.get("observers", "0"))
                if observers >= 2 and int(values.get("escorts", "0")) >= observers:
                    contested_reserve_releases.add(frame)
            elif kind == "SQUAD" and line.split(",", 3)[2] == "MainArmy":
                if values.get("detectionBlocked") == "1":
                    main_detection_blocked.add(frame)
                    blocked_main_groups[frame] += 1
                units = int(values.get("units", "0"))
                if units >= 30:
                    route = values.get("travelReason", "unknown")
                    large_main_routes.add((frame, route))
                    if route == "assemble-at-rally":
                        large_main_rally.add(frame)
                if values.get("travelReason") == "cover-expansion":
                    expansion_cover_routes.add(frame)
            elif kind == "STATE":
                army = int(values.get("army", "0"))
                nexuses = int(values.get("nexuses", "0"))
                peak_army = max(peak_army, army)
                peak_probes = max(peak_probes, int(values.get("probes", "0")))
                if first_three_nexus_frame is None and nexuses >= 3:
                    first_three_nexus_frame = frame
                nexus_match = re.search(r"(?:^|;)Nexus=\d+/(\d+)",
                                        values.get("selfComp", ""))
                if nexus_match:
                    nexus_samples.append((frame, int(nexus_match.group(1))))
                if previous_nexuses is not None and nexuses < previous_nexuses \
                        and army >= 14:
                    nexus_count_drop_with_army.append(frame)
                previous_nexuses = nexuses
    routes = Counter()
    for _, route in large_main_routes:
        routes[route] += 1
    dual_escort_frames = {frame for frame, actors in escort_orders.items()
                          if len(actors) >= 2}
    nexus_frames = [frame for frame, _ in nexus_samples]
    forward_third_routes = set()
    for frame in expansion_cover_routes:
        sample = bisect_right(nexus_frames, frame) - 1
        if sample >= 0 and nexus_samples[sample][1] == 2:
            forward_third_routes.add(frame)
    return {
        "empty_policy_pressure_hold_ticks": len(pressure_held),
        "covered_pressure_release_ticks": len(pressure_released),
        "forward_third_screen_ticks": len(forward_third_routes),
        "forward_third_event_ticks": len(forward_third_screen),
        "contested_reserve_release_ticks": (
            len(contested_reserve_releases) if reserve_telemetry_seen else None),
        "main_detection_blocked_ticks": len(main_detection_blocked),
        "multiple_main_groups_blocked_ticks": sum(
            groups >= 2 for groups in blocked_main_groups.values()),
        "dual_escort_order_frames": len(dual_escort_frames),
        "detector_wait_volley_frames": len(detector_wait_volleys),
        "detector_wait_retreat_frames": len(detector_wait_retreats),
        "dual_escort_with_multiple_blocked_main": sum(
            blocked_main_groups[frame] >= 2 for frame in dual_escort_frames),
        "large_main_rally_ticks": len(large_main_rally),
        "large_main_route_ticks": dict(sorted(routes.items())),
        "peak_army": peak_army,
        "peak_probes": peak_probes,
        "first_three_nexus_frame": first_three_nexus_frame,
        "nexus_count_drop_frames_with_army": nexus_count_drop_with_army,
    }


def campaign(path: Path) -> dict:
    results = path / "server" / "results.jsonl"
    schedule = path / "server" / "games.jsonl"
    expected = sum(bool(line) for line in schedule.read_text().splitlines())
    rows = [json.loads(line) for line in results.read_text().splitlines() if line]
    games = []
    for row in rows:
        if row.get("reportingBot") != "Protodd":
            continue
        game_id = row["gameID"]
        log = (path / "server" / "replays" / "bot-write" /
               f"game-{game_id}" / "Protodd" / "received" / "Protodd.log")
        data = audit_log(log) if log.is_file() else None
        games.append({
            "game_id": game_id,
            "map": row.get("map"),
            "won": row.get("won"),
            "end_type": row.get("gameEndType"),
            "final_frame": row.get("finalFrame"),
            "control": data,
        })
    games.sort(key=lambda game: game["game_id"])
    return {"campaign": str(path.resolve()), "games": games,
            "complete": len(games) == expected and expected > 0 and all(
                game["end_type"] == "NORMAL" and game["control"] is not None
                for game in games)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = campaign(args.campaign)
    payload = json.dumps(report, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()
