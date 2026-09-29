"""Connect full-game losses to army decisions and accepted commands.

Reads local arena logs. ORDER rows are intent-deduplicated by the runtime, so
source counts are sampled accepted traces rather than total commands. Samples
and incidents are diagnostic; they do not estimate any proposed change's effect.
"""

from __future__ import annotations

import argparse
from collections import Counter, deque
import json
import math
from pathlib import Path


WINDOW = 720  # 30 game seconds at 24 frames/s.


def attributes(parts: list[str]) -> dict[str, str]:
    return dict(part.split("=", 1) for part in parts if "=" in part)


def location(value: str) -> tuple[int, int] | None:
    try:
        x, y = value.split("x", 1)
        return int(x), int(y)
    except (ValueError, AttributeError):
        return None


def distance(a: tuple[int, int] | None, b: tuple[int, int] | None) -> float | None:
    return math.dist(a, b) if a is not None and b is not None else None


def audit_log(path: Path) -> dict:
    states: list[dict] = []
    mains: list[dict] = []
    orders: list[dict] = []
    worker_evacuations: list[tuple[int, int]] = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("STATE,"):
                parts = line.rstrip("\n").split(",")
                if len(parts) < 16:
                    continue
                values = attributes(parts[15:])
                states.append({"frame": int(parts[1]), "posture": parts[3],
                               "probes": int(values.get("probes", 0)),
                               "army": int(values.get("army", 0)),
                               "nexuses": int(values.get("nexuses", 0)),
                               "minAttack": int(values.get("minAttack", 0)),
                               "enemyVisibleArmy": int(values.get("enemyVisibleArmy", 0)),
                               "attackTarget": location(values.get("attackTarget", ""))})
            elif line.startswith("SQUAD,"):
                parts = line.rstrip("\n").split(",")
                if len(parts) < 4 or parts[2] != "MainArmy":
                    continue
                values = attributes(parts[3:])
                mains.append({"frame": int(parts[1]), "key": values.get("key", ""),
                              "units": int(values.get("units", 0)),
                              "enemies": int(values.get("enemies", 0)),
                              "ratio": float(values.get("ratio", 0)),
                              "decision": values.get("decision", ""),
                              "route": values.get("travelReason", ""),
                              "blocked": values.get("detectionBlocked") == "1",
                              "center": location(values.get("center", "")),
                              "goal": location(values.get("travelGoal", ""))})
            elif line.startswith("ORDER,"):
                parts = line.rstrip("\n").split(",", 8)
                if len(parts) >= 9:
                    orders.append({"frame": int(parts[1]), "source": parts[7],
                                   "accepted": parts[8] == "1"})
            elif line.startswith("ACTION,"):
                parts = line.rstrip("\n").split(",", 7)
                if (len(parts) >= 7 and parts[3] == "worker-evacuate" and
                        parts[5] == "issued" and parts[6] == "accepted"):
                    worker_evacuations.append((int(parts[1]), int(parts[2])))

    # Use prior state samples to find substantial army and economic losses.
    incidents: list[dict] = []
    recent: deque[dict] = deque()
    last_incident: dict[str, int] = {}
    for state in states:
        while recent and state["frame"] - recent[0]["frame"] > WINDOW:
            recent.popleft()
        for kind, threshold in (("probes", 8), ("army", 8), ("nexuses", 1)):
            if not recent or state["frame"] - last_incident.get(kind, -WINDOW) < WINDOW:
                continue
            peak = max(recent, key=lambda sample: sample[kind])
            loss = peak[kind] - state[kind]
            if loss < threshold or (kind == "nexuses" and state[kind] == 0):
                continue
            preceding = [row for row in mains if peak["frame"] <= row["frame"] <= state["frame"]]
            issued = Counter(order["source"] for order in orders
                             if peak["frame"] <= order["frame"] <= state["frame"]
                             and order["accepted"])
            evacuees = {actor for frame, actor in worker_evacuations
                        if peak["frame"] <= frame <= state["frame"]}
            incidents.append({"kind": kind, "start": peak["frame"],
                              "end": state["frame"], "lost": loss,
                              "before": {key: peak[key] for key in
                                         ("probes", "army", "nexuses", "posture")},
                              "after": {key: state[key] for key in
                                        ("probes", "army", "nexuses", "posture")},
                              "main_routes": dict(Counter(row["route"] for row in preceding)),
                              "main_blocked_samples": sum(row["blocked"] for row in preceding),
                              "main_peak_units": max((row["units"] for row in preceding), default=0),
                              "workers_evacuated": len(evacuees),
                              "accepted_orders": dict(issued.most_common(8))})
            last_incident[kind] = state["frame"]
        recent.append(state)

    # A viable army should make progress during uncontested assault travel.
    stalls: list[dict] = []
    for index, first in enumerate(mains):
        if (first["units"] < 12 or first["enemies"] or first["blocked"] or
                first["route"] not in {"attack-target", "continue-assault", "stalled-terrain-route"}):
            continue
        start_distance = distance(first["center"], first["goal"])
        if start_distance is None or start_distance < 512:
            continue
        later = next((row for row in mains[index + 1:]
                      if row["frame"] >= first["frame"] + WINDOW and
                      row["frame"] <= first["frame"] + WINDOW + 120 and
                      row["units"] >= 12 and row["goal"] == first["goal"] and
                      row["key"] == first["key"]), None)
        if later is None:
            continue
        interval = [row for row in mains[index + 1:]
                    if first["frame"] < row["frame"] <= later["frame"] and
                    row["key"] == first["key"]]
        if (not interval or interval[-1] is not later or
                any(row["units"] < 12 or row["enemies"] or row["blocked"] or
                    row["goal"] != first["goal"] or
                    row["route"] not in {"attack-target", "continue-assault", "stalled-terrain-route"}
                    for row in interval)):
            continue
        end_distance = distance(later["center"], later["goal"])
        if end_distance is None or start_distance - end_distance >= 128:
            continue
        if stalls and first["frame"] - stalls[-1]["end"] < WINDOW:
            continue
        stalls.append({"start": first["frame"], "end": later["frame"],
                       "units": first["units"], "route": first["route"],
                       "distance_before": round(start_distance),
                       "distance_after": round(end_distance)})

    return {"states": len(states), "main_samples": len(mains),
            "workers_evacuated": len({actor for _, actor in worker_evacuations}),
            "accepted_order_sources": dict(Counter(order["source"] for order in orders
                                           if order["accepted"]).most_common(20)),
            "incidents": incidents, "assault_stalls": stalls}


def campaign(root: Path) -> dict:
    results = root / "server" / "results.jsonl"
    games = []
    for line in results.read_text(encoding="utf-8").splitlines():
        row = json.loads(line)
        if row.get("reportingBot") != "Protodd":
            continue
        game_id = row["gameID"]
        log = root / "server" / "replays" / "bot-write" / f"game-{game_id}" / \
              "Protodd" / "received" / "Protodd.log"
        games.append({"game_id": game_id, "opponent": row.get("opponentBot"),
                      "map": row.get("map"), "won": row.get("won"),
                      "end_type": row.get("gameEndType"),
                      "audit": audit_log(log) if log.is_file() else None})
    return {"campaign": str(root.resolve()), "games": games}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path, nargs="+")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    reports = [campaign(path) for path in args.campaign]
    payload = json.dumps(reports, indent=2)
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(payload + "\n", encoding="utf-8")
    print(payload)


if __name__ == "__main__":
    main()
