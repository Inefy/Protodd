"""Audit whole-game shard label coverage without mixing validation into training."""
from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path

from .whole_game_release import key, verified_receipt


def coverage(release_dir: Path, verify_hashes=True):
    release_dir = Path(release_dir)
    identity = json.loads((release_dir / "identity.json").read_text())
    progress = release_dir / "progress.json"
    counters = {split: dict(games=0, frames=0, observations=0, commands=0,
                            candidates=0, checkpoints=0, matchups=Counter(),
                            domains=Counter(), kinds=Counter(), evidence=Counter())
                for split in ("train", "validation")}
    seen = 0
    # Receipts are hash-bound to the release identity by whole_game_release.
    identity_sha = None
    if progress.exists():
        # An in-progress release has no release.json yet; obtain the identity
        # digest using the same canonical encoding as its builder.
        import hashlib
        identity_sha = hashlib.sha256((json.dumps(identity, sort_keys=True,
                separators=(",", ":")) + "\n").encode()).hexdigest()
    else:
        identity_sha = json.loads((release_dir / "release.json").read_text())["identity_sha256"]
    for record in identity["selected"]:
        destination = release_dir / "games" / key(record)
        if not destination.exists():
            continue
        receipt = verified_receipt(destination, record, identity_sha) if verify_hashes else json.loads(
            (destination / "receipt.json").read_text())
        split = record["split"]
        if split not in counters:
            raise ValueError("sealed split reached coverage audit")
        c = counters[split]
        report = receipt["report"]
        c["games"] += 1
        c["frames"] += report["valid_through_frame"]
        c["observations"] += report["counts"]["observations"]
        c["commands"] += report["counts"]["commands"]
        c["candidates"] += report["labels"]["candidate_labels"]
        c["checkpoints"] += report["checkpoints_matched"]
        c["matchups"][record["matchup"]] += 1
        c["domains"].update(report["labels"]["domains"])
        c["kinds"].update(report["labels"]["kinds"])
        c["evidence"].update(report["command_evidence"])
        seen += 1
    return dict(schema="protodd-whole-game-coverage-v1", completed_games=seen,
                selected_games=len(identity["selected"]), splits=counters,
                warnings=["candidate labels confirm immediate transitions, not eventual completion",
                          "domain coverage does not establish strategic or combat competence"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("--no-verify-hashes", action="store_true")
    args = parser.parse_args()
    print(json.dumps(coverage(args.release, not args.no_verify_hashes), indent=2, default=dict))


if __name__ == "__main__":
    main()
