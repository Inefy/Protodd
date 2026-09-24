"""Causal readers for verified, compressed whole-game train/validation shards."""
from __future__ import annotations

import gzip
import hashlib
import json
from pathlib import Path

from .whole_game_release import key, verified_receipt
from .whole_game_sequences import trajectory_rows


def selected_shards(release_dir, split, *, require_complete=False, verify_hashes=True,
                    game_ids=None):
    if split not in ("train", "validation"):
        raise ValueError("only train or validation may be loaded; final test is sealed")
    release_dir = Path(release_dir)
    identity = json.loads((release_dir / "identity.json").read_text())
    if require_complete:
        release = json.loads((release_dir / "release.json").read_text())
        if release.get("complete") is not True:
            raise ValueError("whole-game release incomplete")
    identity_sha = hashlib.sha256((json.dumps(identity, sort_keys=True,
                          separators=(",", ":")) + "\n").encode()).hexdigest()
    for record in identity["selected"]:
        if record["split"] != split:
            continue
        if game_ids is not None and record["game_id"] not in game_ids:
            continue
        directory = release_dir / "games" / key(record)
        if not directory.exists():
            if require_complete:
                raise ValueError(f"missing shard: {key(record)}")
            continue
        receipt = verified_receipt(directory, record, identity_sha) if verify_hashes else json.loads(
            (directory / "receipt.json").read_text())
        yield record, directory, receipt


def load_terrain(directory):
    with gzip.open(Path(directory) / "terrain.json.gz", "rt", encoding="utf-8") as stream:
        return json.load(stream)


def trajectory_shard(directory, window=24):
    directory = Path(directory)
    with gzip.open(directory / "summary.json.gz", "rt", encoding="utf-8") as stream:
        summary = json.load(stream)
    with gzip.open(directory / "imitation-labels.jsonl.gz", "rt", encoding="utf-8") as labels, \
            gzip.open(directory / "observations.jsonl.gz", "rt", encoding="utf-8") as observations:
        yield from trajectory_rows(summary, labels, observations, window)
