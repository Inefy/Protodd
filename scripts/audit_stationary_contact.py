"""Count potential stationary-contact exposure in explicitly supplied archived logs.

This uses legal ENTITY observations and sampled SQUAD membership, not replays.
Counts establish exposure only: they do not prove movement intent or win impact.
"""
import argparse
import json
import math
from pathlib import Path


def fields(parts):
    return dict(part.split("=", 1) for part in parts if "=" in part)


def audit(path):
    snapshots, squads, summaries = {}, {}, []
    with path.open(encoding="utf-8", errors="replace") as source:
        for line in source:
            parts = line.rstrip().split(",")
            if parts[0] == "SUMMARY":
                summaries.append(fields(parts[1:]))
            elif parts[0] == "SQUAD":
                squads.setdefault(int(parts[1]), []).append(fields(parts[3:]))
            elif parts[0] == "ENTITY" and parts[4] in (
                "Photon Cannon", "Zealot", "Zergling", "Hydralisk"
            ):
                snapshots.setdefault(int(parts[1]), []).append((parts, fields(parts[14:])))
    exposure_frames, evaluation_frames, examples = set(), set(), []
    for frame, units in snapshots.items():
        cannons = [(p, f) for p, f in units if p[2] == "self" and p[4] == "Photon Cannon"
                   and f.get("completed") == "1" and f.get("powered") == "1"]
        enemies = [(p, f) for p, f in units if p[2] == "enemy" and p[4] != "Photon Cannon"
                   and p[12] == "1" and f.get("completed") == "1"]
        for cannon, _ in cannons:
            for enemy, attributes in enemies:
                separation = math.hypot(int(cannon[5]) - int(enemy[5]),
                                        int(cannon[6]) - int(enemy[6]))
                # A margin for collision bounds keeps the centre-distance
                # screen outside the Cannon's edge-to-edge firing range.
                if not (272 < separation < 640 and
                        float(attributes.get("topSpeed", "0")) > 0 and
                        int(attributes.get("groundRange", "999")) < 224):
                    continue
                exposure_frames.add(frame)
                selected = [s for s in squads.get(frame, []) if s.get("simulation") == "1"
                            and int(s.get("enemies", "0")) > 0
                            and cannon[3] in s.get("members", "").split(";")]
                if selected:
                    evaluation_frames.add(frame)
                    if len(examples) < 3:
                        examples.append({"frame": frame, "cannon": cannon[3], "enemy": enemy[3],
                                         "enemy_kind": enemy[4], "centre_distance": round(separation, 1),
                                         "enemy_speed": attributes["topSpeed"],
                                         "squad_ratio": selected[0].get("ratio"),
                                         "squad_decision": selected[0].get("decision")})
    return {"log": str(path.resolve()), "summary": summaries,
            "visible_candidate_pair_frames": len(exposure_frames),
            "pair_frames_with_cannon_in_simulated_combat_squad": len(evaluation_frames),
            "examples": examples,
            "limitation": "Potential exposure only; samples do not prove approach intent, enemy membership, or live strength."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("logs", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([audit(path) for path in args.logs], indent=2))
