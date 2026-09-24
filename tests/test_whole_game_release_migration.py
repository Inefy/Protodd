import gzip
import json

import pytest

from training.whole_game_pilot import sha256
from training.whole_game_release import ARTIFACTS, SCHEMA, key
from training.whole_game_release_migration import identity_digest, migrate_verified_shards


def _fixture(tmp_path):
    old, new = tmp_path / "old", tmp_path / "new"
    old.mkdir()
    new.mkdir()
    record = dict(game_id="game:" + "a" * 64, split="train", matchup="PvT",
                  path="PvT/sample.rep", replay_sha256="b" * 64,
                  perspective=0, valid_through_frame=12000)
    identity = dict(schema=SCHEMA, extractor_schema="native", source_release_manifest_sha256="m",
                    per_matchup=None, max_frames=None, selected=[record],
                    input_sha256={"C:/data/StarDat.mpq": "same", "C:/code/whole_game_labels.py": "old"})
    updated = dict(identity, input_sha256=dict(identity["input_sha256"],
                                               **{"C:/code/whole_game_labels.py": "new"}))
    (old / "identity.json").write_text(json.dumps(identity))
    (new / "identity.json").write_text(json.dumps(updated))
    source = old / "games" / key(record)
    source.mkdir(parents=True)
    hashes = {}
    for name in ARTIFACTS:
        target = source / (name + ".gz")
        target.write_bytes(gzip.compress(b"{}\n", mtime=0))
        hashes[name] = sha256(target)
    (source / "receipt.json").write_text(json.dumps(dict(schema=SCHEMA, identity_sha256=identity_digest(identity),
                                                         record=record, artifact_sha256=hashes)))
    return old, new, record


def test_migration_reissues_receipts_and_is_resumable(tmp_path):
    old, new, record = _fixture(tmp_path)
    assert migrate_verified_shards(old, new)["migrated"] == 1
    destination = new / "games" / key(record)
    receipt = json.loads((destination / "receipt.json").read_text())
    assert receipt["identity_sha256"] == identity_digest(json.loads((new / "identity.json").read_text()))
    assert receipt["migrated_from_identity_sha256"] == identity_digest(json.loads((old / "identity.json").read_text()))
    assert json.loads((new / "migration.json").read_text())["migrated"] == 1
    assert migrate_verified_shards(old, new)["already_present"] == 1


def test_migration_rejects_changed_replay_inputs(tmp_path):
    old, new, _ = _fixture(tmp_path)
    updated = json.loads((new / "identity.json").read_text())
    updated["input_sha256"]["C:/data/StarDat.mpq"] = "different"
    (new / "identity.json").write_text(json.dumps(updated))
    with pytest.raises(ValueError, match="extraction input"):
        migrate_verified_shards(old, new)


def test_migration_rejects_damaged_source_shard(tmp_path):
    old, new, record = _fixture(tmp_path)
    (old / "games" / key(record) / "commands.jsonl.gz").write_bytes(b"damaged")
    with pytest.raises(ValueError, match="damaged shard"):
        migrate_verified_shards(old, new)
