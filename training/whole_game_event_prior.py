"""Measure the natural timing-label prior in a frozen train-only replay cohort."""
from __future__ import annotations

import argparse
from collections import Counter
import gzip
import hashlib
import json
import math
from pathlib import Path

from .whole_game_fit import MATCHUPS
from .whole_game_shards import selected_shards
from .whole_game_stream_fit import verify_chunks


SCHEMA = "protodd-whole-game-event-prior-v1"


def count_game(directory, window=24):
    """Count exactly the complete cadence windows used by trajectory_rows."""
    directory = Path(directory)
    if window < 1:
        raise ValueError("window must be positive")
    with gzip.open(directory / "summary.json.gz", "rt", encoding="utf8") as stream:
        summary = json.load(stream)
    if summary.get("complete") is not True or summary.get("valid_through_frame", -1) < 0:
        raise ValueError("incomplete replay summary")
    windows = summary["valid_through_frame"] // window
    positive = set()
    with gzip.open(directory / "imitation-labels.jsonl.gz", "rt", encoding="utf8") as stream:
        for line in stream:
            frame = json.loads(line)["frame"]
            index = frame // window
            if 0 <= index < windows:
                positive.add(index)
    return windows, len(positive)


def measure(release, selection, output):
    release, selection, output = map(Path, (release, selection, output))
    if output.exists():
        raise FileExistsError(output)
    identity_bytes = (release / "identity.json").read_bytes()
    selection_bytes = selection.read_bytes()
    cohort = json.loads(selection_bytes.decode("utf8"))
    identity_sha = hashlib.sha256(identity_bytes).hexdigest()
    if cohort["source_identity_sha256"] != identity_sha:
        raise ValueError("frozen cohort belongs to another replay release")
    verify_chunks(release, cohort["groups"],
                  games_per_matchup=cohort["games_per_matchup"],
                  chunk_games_per_matchup=cohort["chunk_games_per_matchup"])
    chosen = {game_id for group in cohort["groups"] for ids in group.values()
              for game_id in ids}
    counts = {matchup: Counter() for matchup in MATCHUPS}
    found = set()
    for record, directory, _ in selected_shards(release, "train", game_ids=chosen):
        windows, events = count_game(directory)
        counts[record["matchup"]].update(games=1, windows=windows, events=events)
        found.add(record["game_id"])
    if found != chosen or any(counts[matchup]["games"] != cohort["games_per_matchup"]
                              for matchup in MATCHUPS):
        raise ValueError("train-only cohort is incomplete")
    windows = sum(item["windows"] for item in counts.values())
    events = sum(item["events"] for item in counts.values())
    if not 0 < events < windows:
        raise ValueError("timing prior requires both event classes")
    rate = events / windows
    # Stream training cycles positive/negative event buckets equally, so its
    # effective event prior is 0.5. This train-only logit correction restores
    # the observed prior without learning from validation labels.
    correction = math.log(rate / (1 - rate))
    report = dict(schema=SCHEMA, train_only=True, source_identity_sha256=identity_sha,
                  selection_sha256=hashlib.sha256(selection_bytes).hexdigest(),
                  games=len(found), windows=windows, events=events, event_rate=rate,
                  balanced_training_rate=0.5, logit_prior_correction=correction,
                  by_matchup={matchup: dict(counts[matchup]) for matchup in MATCHUPS})
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("selection", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(measure(args.release, args.selection, args.output), indent=2))


if __name__ == "__main__":
    main()
