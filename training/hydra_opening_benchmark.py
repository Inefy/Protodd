"""Describe train-split Protoss openings that saw a large early Hydra group.

This is a survivor-conditioned observation benchmark, not a counterfactual
claim that copying those builds would beat the same opponent.
"""

import argparse
from collections import Counter
import gzip
import hashlib
import json
from pathlib import Path

from .human_opening_benchmark import summarize


FIELDS = ("own_complete/probe", "own_complete/nexus",
          "own_complete/gateway", "own_complete/zealot",
          "own_complete/dragoon", "own_complete/photon_cannon",
          "own_incomplete/photon_cannon", "own_complete/reaver",
          "own_complete/high_templar", "enemy_visible/hydralisk")
FRAMES = (7200, 8400, 9600, 10800, 12000)


def benchmark(release, games_dir, schema, sample=256, threshold=10):
    games = json.loads(Path(release).read_text(encoding="utf-8"))["games"]
    selected = sorted((game for game in games if game["split"] == "train" and
                       game["path"].startswith("ZvP/")),
                      key=lambda game: hashlib.sha256(
                          game["game_id"].encode("utf-8")).hexdigest())[:sample]
    features = json.loads(Path(schema).read_text(encoding="utf-8"))["features"]
    indices = {item["name"]: (index, item["scale"])
               for index, item in enumerate(features)}
    if missing := set(FIELDS) - indices.keys():
        raise ValueError(f"schema lacks features: {sorted(missing)}")
    cohort = []
    unavailable = Counter()
    for game in selected:
        path = Path(games_dir) / game["replay_sha256"] / "samples.jsonl.gz"
        if not path.is_file():
            unavailable["missing_file"] += 1
            continue
        observed = {}
        with gzip.open(path, "rt", encoding="utf-8") as stream:
            for line in stream:
                row = json.loads(line)
                frame = row["frame"]
                if frame in FRAMES and frame not in observed:
                    vector = row["features"]
                    observed[frame] = {field: round(vector[indices[field][0]] *
                                                    indices[field][1], 3)
                                       for field in FIELDS}
                if frame > FRAMES[-1] + 24:
                    break
        signal = observed.get(9600)
        if signal is None:
            unavailable["no_frame_9600"] += 1
        elif signal["enemy_visible/hydralisk"] >= threshold:
            cohort.append(dict(game_id=game["game_id"],
                               replay_sha256=game["replay_sha256"],
                               observations=observed))
    by_frame = {}
    for frame in FRAMES:
        rows = [game["observations"][frame] for game in cohort
                if frame in game["observations"]]
        by_frame[str(frame)] = dict(n=len(rows),
                                    fields={field: summarize([row[field] for row in rows])
                                            for field in FIELDS} if rows else {})
    return dict(schema="protodd-train-hydra-opening-benchmark-v1",
                release=str(Path(release).resolve()),
                games_dir=str(Path(games_dir).resolve()),
                feature_schema=str(Path(schema).resolve()),
                selection="smallest SHA-256 of frozen train ZvP game_id",
                selected_games=len(selected),
                condition=dict(frame=9600, feature="enemy_visible/hydralisk",
                               minimum=threshold),
                unavailable=dict(unavailable), cohort=cohort, frames=by_frame,
                caveat="Observed surviving games; no causal win claim")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--release", type=Path, required=True)
    parser.add_argument("--games-dir", type=Path, required=True)
    parser.add_argument("--schema", type=Path,
                        default=Path("training/schema_v2.json"))
    parser.add_argument("--sample", type=int, default=256)
    parser.add_argument("--threshold", type=int, default=10)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = benchmark(args.release, args.games_dir, args.schema,
                       args.sample, args.threshold)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(dict(selected=result["selected_games"],
                          exposed=len(result["cohort"]),
                          unavailable=result["unavailable"])))
