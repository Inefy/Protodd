"""Summarize legal opening observations from frozen train-split replays.

This is a per-game descriptive benchmark, not a replay policy evaluation or a
strength claim. It never opens validation or final-test samples.
"""

from __future__ import annotations

import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path
import statistics


FIELDS = (
    "minerals", "gas", "supply_used_doubled", "own_complete/probe",
    "own_complete/nexus", "own_incomplete/nexus", "own_complete/gateway",
    "own_complete/zealot", "own_complete/dragoon",
    "own_complete/forge", "own_complete/pylon", "own_complete/assimilator",
    "own_complete/photon_cannon", "own_complete/cybernetics_core",
    "own_complete/robotics_facility", "own_complete/observatory",
    "own_complete/stargate", "own_complete/templar_archives",
    "own_complete/robotics_support_bay", "own_complete/high_templar",
    "own_complete/reaver", "own_complete/corsair",
    "own_tech_level/psionic_storm", "own_tech_in_progress/psionic_storm",
)


def summarize(values: list[float]) -> dict[str, float]:
    ordered = sorted(values)
    return {"n": len(ordered), "p25": ordered[len(ordered) // 4],
            "median": statistics.median(ordered),
            "p75": ordered[(3 * len(ordered)) // 4]}


def benchmark(release: Path, games_dir: Path, schema: Path,
              matchup: str, sample: int, frames: tuple[int, ...]) -> dict:
    games = json.loads(release.read_text(encoding="utf-8"))["games"]
    eligible = [game for game in games if game["split"] == "train" and
                game["path"].startswith(f"{matchup}/")]
    eligible.sort(key=lambda game: hashlib.sha256(
        game["game_id"].encode("utf-8")).hexdigest())
    selected = eligible[:sample]
    features = json.loads(schema.read_text(encoding="utf-8"))["features"]
    indices = {item["name"]: (index, item["scale"])
               for index, item in enumerate(features)}
    if missing := set(FIELDS) - indices.keys():
        raise ValueError(f"schema lacks requested fields: {sorted(missing)}")
    observations: dict[int, dict[str, list[float]]] = {
        frame: {field: [] for field in FIELDS} for frame in frames}
    unavailable = Counter()
    for game in selected:
        path = games_dir / game["replay_sha256"] / "samples.jsonl.gz"
        if not path.is_file():
            unavailable["missing_file"] += 1
            continue
        found = {}
        with gzip.open(path, "rt", encoding="utf-8") as source:
            for line in source:
                row = json.loads(line)
                frame = row["frame"]
                if frame > max(frames) + 24:
                    break
                if frame in observations and frame not in found:
                    found[frame] = row["features"]
        for frame in frames:
            vector = found.get(frame)
            if vector is None:
                unavailable[f"no_frame_{frame}"] += 1
                continue
            for field in FIELDS:
                index, scale = indices[field]
                observations[frame][field].append(round(vector[index] * scale, 3))
    return {
        "source": str(release), "matchup": matchup,
        "selection": "smallest SHA-256 of frozen train game_id",
        "eligible_train_games": len(eligible), "selected_games": len(selected),
        "unavailable": dict(unavailable),
        "frames": {str(frame): {field: summarize(values) for field, values in fields.items()
                                if values}
                   for frame, fields in observations.items()},
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--games-dir", type=Path, required=True)
    parser.add_argument("--schema", type=Path, default=Path("training/schema_v2.json"))
    parser.add_argument("--matchup", default="ZvP")
    parser.add_argument("--sample", type=int, default=256)
    parser.add_argument("--frames", type=int, nargs="+",
                        default=[4800, 6000, 6600, 7200, 8400, 9600,
                                 10800, 12000, 13200])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = benchmark(args.release, args.games_dir, args.schema, args.matchup,
                       args.sample, tuple(args.frames))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"selected_games": report["selected_games"],
                      "unavailable": report["unavailable"],
                      "output": str(args.output)}))


if __name__ == "__main__":
    main()
