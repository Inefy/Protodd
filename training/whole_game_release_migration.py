"""Reuse verified, unaffected shards in a newly pinned extraction release.

The destination identity must already exist. Each source artifact is verified
before hard-linking, and the new receipt is committed last. Failed source games
remain absent so the new extractor can process them with its pinned code.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
from uuid import uuid4

from .whole_game_pilot import sha256
from .whole_game_release import ARTIFACTS, SCHEMA, atomic_json, commit_shard, key, verified_receipt


def identity_digest(identity):
    payload = (json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n").encode()
    return hashlib.sha256(payload).hexdigest()


def _read_identity(root):
    return json.loads((root / "identity.json").read_text(encoding="utf-8"))


def _check_compatible(old, new):
    for field in ("schema", "extractor_schema", "source_release_manifest_sha256",
                  "per_matchup", "max_frames", "selected"):
        if old.get(field) != new.get(field):
            raise ValueError(f"release migration changed {field}")
    # Only the labeler and release driver may differ. Reused compressed shards
    # therefore retain identical native playback, assets, and replay inputs.
    mutable_sources = {"whole_game_labels.py", "whole_game_pilot.py", "whole_game_release.py",
                       "whole_game_actor_dedupe.py", "whole_game_release_migration.py"}
    for path, digest in old["input_sha256"].items():
        if Path(path).name not in mutable_sources and new["input_sha256"].get(path) != digest:
            raise ValueError(f"release migration changed extraction input: {path}")
    for path in new["input_sha256"].keys() - old["input_sha256"].keys():
        if Path(path).name not in mutable_sources:
            raise ValueError(f"release migration added extraction input: {path}")


def migrate_verified_shards(old_root, new_root):
    old_root, new_root = Path(old_root).resolve(), Path(new_root).resolve()
    if old_root == new_root:
        raise ValueError("migration source and destination are identical")
    old, new = _read_identity(old_root), _read_identity(new_root)
    _check_compatible(old, new)
    old_sha, new_sha = identity_digest(old), identity_digest(new)
    old_games, new_games = old_root / "games", new_root / "games"
    copied, already, missing = 0, 0, 0
    for record in new["selected"]:
        source = old_games / key(record)
        if not (source / "receipt.json").exists():
            missing += 1
            continue
        original = verified_receipt(source, record, old_sha)
        if original is None:
            raise ValueError(f"missing source receipt: {source}")
        destination = new_games / key(record)
        if destination.exists():
            if verified_receipt(destination, record, new_sha) is None:
                raise ValueError(f"uncommitted destination shard: {destination}")
            already += 1
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging = destination.with_name(destination.name + "." + uuid4().hex + ".tmp")
        staging.mkdir()
        try:
            for name in ARTIFACTS:
                os.link(source / (name + ".gz"), staging / (name + ".gz"))
            receipt = dict(original)
            receipt["identity_sha256"] = new_sha
            receipt["migrated_from_identity_sha256"] = old_sha
            atomic_json(staging / "receipt.json", receipt)
            commit_shard(staging, destination, receipt, new_games)
        finally:
            if staging.exists():
                shutil.rmtree(staging)
        copied += 1
    report = dict(schema="protodd-whole-game-migration-v1", migrated=copied,
                  already_present=already, pending=missing,
                  source_identity_sha256=old_sha, destination_identity_sha256=new_sha,
                  migration_source_sha256=sha256(__file__))
    atomic_json(new_root / "migration.json", report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("old_release", type=Path)
    parser.add_argument("new_release", type=Path)
    args = parser.parse_args()
    print(json.dumps(migrate_verified_shards(args.old_release, args.new_release), indent=2))


if __name__ == "__main__":
    main()
