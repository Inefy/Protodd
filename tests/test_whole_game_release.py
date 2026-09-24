import gzip
import json
from pathlib import Path
from unittest.mock import patch

import pytest

from training.whole_game_release import ARTIFACTS, SCHEMA, commit_shard, select_games, verified_receipt
from training.whole_game_pilot import sha256
from training.whole_game_shards import selected_shards, trajectory_shard


def _game(split, races=("P", "T"), quality=("qualified_ladder", "unknown")):
    return dict(game_id="game:" + "a" * 64, path="PvT/sample.rep", replay_sha256="b" * 64,
                split=split, races=races, player_quality=quality, valid_through_frame=12000)


def test_selector_never_opens_sealed_test_and_keeps_one_perspective():
    train = _game("train", ("P", "P"), ("qualified_ladder", "qualified_ladder"))
    validation = _game("validation", ("T", "P"), ("unknown", "qualified_ladder"))
    validation["game_id"] = "game:" + "c" * 64
    sealed = _game("test")
    sealed["path"] = "does-not-exist.rep"
    result = select_games({"games": [sealed, train, validation]})
    assert [(r["split"], r["matchup"], r["perspective"]) for r in result] == [
        ("train", "PvP", 0), ("validation", "PvT", 1)]


def test_receipt_detects_corrupt_compressed_artifact(tmp_path: Path):
    record = dict(game_id="game:" + "a" * 64, split="train", matchup="PvT",
                  path="PvT/sample.rep", replay_sha256="b" * 64,
                  perspective=0, valid_through_frame=12000)
    hashes = {}
    for name in ARTIFACTS:
        target = tmp_path / (name + ".gz")
        target.write_bytes(gzip.compress(b"{}\n", mtime=0))
        hashes[name] = sha256(target)
    receipt = dict(schema=SCHEMA, identity_sha256="pin", record=record,
                   artifact_sha256=hashes)
    (tmp_path / "receipt.json").write_text(json.dumps(receipt))
    assert verified_receipt(tmp_path, record, "pin") == receipt
    (tmp_path / "terrain.json.gz").write_bytes(b"damaged")
    with pytest.raises(ValueError, match="damaged shard"):
        verified_receipt(tmp_path, record, "pin")


def test_compressed_trajectory_and_sealed_split_guard():
    release = Path(__file__).resolve().parents[1] / "artifacts/replay-learning/whole-game-release-benchmark2-v32-20260922"
    if not release.exists():
        pytest.skip("local integration benchmark unavailable")
    record, directory, receipt = next(selected_shards(release, "train"))
    rows = list(trajectory_shard(directory))
    assert len(rows) == receipt["report"]["counts"]["observations"]
    assert sum(supervision["action"] is not None for _, supervision in rows) == receipt["report"]["labels"]["candidate_labels"]
    assert all(row["perspective"] == record["perspective"] for row, _ in rows)
    assert [r["game_id"] for r, _, _ in selected_shards(release, "train", game_ids={record["game_id"]})] == [record["game_id"]]
    with pytest.raises(ValueError, match="sealed"):
        list(selected_shards(release, "test"))


def test_receipt_last_copy_fallback_for_windows_directory_lock(tmp_path: Path):
    root = tmp_path / "games"
    root.mkdir()
    staged = tmp_path / "staged"
    staged.mkdir()
    target = staged / "summary.json.gz"
    target.write_bytes(gzip.compress(b"{}\n", mtime=0))
    receipt = {"artifact_sha256": {"summary.json": sha256(target)}}
    destination = root / "train" / "PvT" / "game"
    destination.parent.mkdir(parents=True)
    with patch("training.whole_game_release.os.replace", side_effect=PermissionError("locked")):
        # atomic_json uses os.replace, so supply the committed marker directly
        # once the directory rename has fallen back to file copies.
        with patch("training.whole_game_release.atomic_json", side_effect=lambda p, data: p.write_text(json.dumps(data))), \
                patch("training.whole_game_release.time.sleep"):
            commit_shard(staged, destination, receipt, root)
    assert sha256(destination / "summary.json.gz") == receipt["artifact_sha256"]["summary.json"]
    assert json.loads((destination / "receipt.json").read_text()) == receipt
