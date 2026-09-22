"""Validate already-extracted, perspective-safe samples into a streaming SQLite dataset.

This does not extract .rep files or certify a replay engine. Validation evidence
must come from the extractor's audit. Unknown quality and evaluation bot traces
cannot enter imitation training.
"""
import argparse
import gzip
from collections import Counter
import json
import math
from pathlib import Path
import re
import sqlite3
import struct

from .schema import load_schema, sha256

QUALITIES = {"verified_pro": 1.0, "qualified_ladder": 0.5}
SPLITS = {"train", "validation", "test"}


def integer(value, label, minimum=0):
    if type(value) is not int or not minimum <= value <= 2_147_483_647:
        raise ValueError(f"invalid {label}")
    return value


def identifier(value, label):
    if not isinstance(value, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,160}", value):
        raise ValueError(f"invalid {label}")
    return value


def validate_manifest(manifest, schema, allow_synthetic=False):
    if manifest.get("schema") != schema["version"] or manifest.get("fingerprint") != schema["fingerprint"]:
        raise ValueError("manifest schema mismatch")
    if manifest.get("source") != "human_replay" and not (
            allow_synthetic and manifest.get("source") == "synthetic_test"):
        raise ValueError("only validated human replays or explicitly enabled synthetic tests are accepted")
    if not manifest.get("extractor") or not manifest.get("audit_id"):
        raise ValueError("extractor version and audit_id required")
    validation = manifest.get("validation", {})
    if set(validation) != {"playback", "perspective", "actions"} or any(v is not True for v in validation.values()):
        raise ValueError("playback, perspective and action validation required")
    games, groups, hashes = {}, {}, {}
    for game in manifest.get("games", []):
        gid = identifier(game.get("game_id"), "game_id")
        group = identifier(game.get("duplicate_group"), "duplicate_group")
        split = game.get("split")
        digest = game.get("replay_sha256", "")
        if split not in SPLITS or not re.fullmatch(r"[0-9a-f]{64}", digest):
            raise ValueError("invalid split or replay hash")
        if gid in games or digest in hashes:
            raise ValueError("duplicate game or replay hash; canonicalize copies before preparing")
        if group in groups and groups[group] != split:
            raise ValueError("duplicate group crosses data splits")
        if len(game.get("races", [])) != 2 or any(r not in ("P", "T", "Z") for r in game["races"]):
            raise ValueError("exactly two resolved races required")
        if len(game.get("player_quality", [])) != 2:
            raise ValueError("quality must be assigned separately to both player slots")
        integer(game.get("valid_through_frame"), "valid_through_frame")
        games[gid] = game
        groups[group] = split
        hashes[digest] = gid
    if not games:
        raise ValueError("empty game manifest")
    return games


def validate_sample(row, games, schema):
    expected = {"game_id", "perspective", "frame", "action_frame", "features", "allowed_actions", "action", "confidence"}
    if set(row) != expected:
        raise ValueError("sample fields mismatch; privileged state belongs outside this dataset")
    game = games.get(row["game_id"])
    if game is None:
        raise ValueError("sample game missing from manifest")
    perspective = integer(row["perspective"], "perspective")
    if perspective not in (0, 1) or game["races"][perspective] != "P":
        raise ValueError("a Protoss player perspective is required")
    quality = game["player_quality"][perspective]
    if quality not in QUALITIES:
        raise ValueError("acting player's quality is unknown or unqualified")
    frame = integer(row["frame"], "frame")
    action_frame = integer(row["action_frame"], "action_frame")
    if not frame <= action_frame <= game["valid_through_frame"]:
        raise ValueError("label precedes observation or exceeds the validated prefix")
    features = row["features"]
    if len(features) != len(schema["features"]) or any(
            type(v) not in (int, float) or not math.isfinite(v) or not 0 <= v <= 16 for v in features):
        raise ValueError("features must match the schema and be finite encoded values in [0,16]")
    # The sample timestamp must match the C++ encoder, not a future observation.
    if abs(features[0] - min(16, frame / 86400)) > 1e-6:
        raise ValueError("feature frame differs from decision frame")
    actions = schema["actions"]
    allowed = row["allowed_actions"]
    if not isinstance(allowed, list) or not allowed or any(type(a) is not str for a in allowed):
        raise ValueError("allowed_actions must be a nonempty named list")
    if len(set(allowed)) != len(allowed) or any(a not in actions for a in allowed):
        raise ValueError("unknown or duplicate allowed action")
    if "wait" not in allowed or row["action"] not in allowed:
        raise ValueError("wait and the labeled action must be allowed")
    confidence = row["confidence"]
    if type(confidence) not in (int, float) or not math.isfinite(confidence) or not 0 < confidence <= 1:
        raise ValueError("confidence must be in (0,1]")
    mask = sum(1 << actions.index(a) for a in allowed)
    blob = struct.pack(f"<{len(features)}f", *features)
    return (row["game_id"], perspective, frame, action_frame, blob, mask,
            actions.index(row["action"]), confidence * QUALITIES[quality])


def prepare(manifest_path, samples_path, output, allow_synthetic=False):
    manifest_path, samples_path, output = map(Path, (manifest_path, samples_path, output))
    schema = load_schema()
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    games = validate_manifest(manifest, schema, allow_synthetic)
    partial = output.with_name(output.name + ".partial")
    if output.exists() or partial.exists():
        raise ValueError("output or unfinished preparation exists; use a new output path")
    output.parent.mkdir(parents=True, exist_ok=True)
    db = sqlite3.connect(partial)
    counts = Counter()
    try:
        db.execute("PRAGMA foreign_keys=ON")
        db.executescript("""
            CREATE TABLE metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL);
            CREATE TABLE games (game_id TEXT PRIMARY KEY, split TEXT NOT NULL,
                matchup TEXT NOT NULL, sample_count INTEGER NOT NULL DEFAULT 0);
            CREATE INDEX game_split ON games(split);
            CREATE TABLE samples (game_id TEXT NOT NULL REFERENCES games(game_id),
                perspective INTEGER NOT NULL, frame INTEGER NOT NULL, action_frame INTEGER NOT NULL,
                features BLOB NOT NULL, mask INTEGER NOT NULL, action INTEGER NOT NULL, weight REAL NOT NULL,
                PRIMARY KEY(game_id,perspective,frame));
        """)
        for gid, game in games.items():
            other = next((r for r in game["races"] if r != "P"), "P")
            db.execute("INSERT INTO games(game_id,split,matchup) VALUES(?,?,?)", (gid, game["split"], "Pv" + other))
        opener = gzip.open if samples_path.suffix == ".gz" else open
        with opener(samples_path, "rt", encoding="utf-8") as source:
            for line_number, line in enumerate(source, 1):
                try:
                    row = json.loads(line)
                    values = validate_sample(row, games, schema)
                    db.execute("INSERT INTO samples VALUES(?,?,?,?,?,?,?,?)", values)
                    db.execute("UPDATE games SET sample_count=sample_count+1 WHERE game_id=?", (values[0],))
                    counts[games[values[0]]["split"]] += 1
                except (ValueError, TypeError, KeyError, sqlite3.IntegrityError) as error:
                    raise ValueError(f"sample line {line_number}: {error}") from error
        if not counts["train"] or not counts["validation"]:
            raise ValueError("nonempty train and validation samples required")
        metadata = dict(format_version=1, schema=schema, source=manifest["source"],
                        manifest_sha256=sha256(manifest_path), samples_sha256=sha256(samples_path),
                        manifest=manifest, counts=dict(counts))
        db.executemany("INSERT INTO metadata VALUES(?,?)", [(k, json.dumps(v)) for k, v in metadata.items()])
        db.commit()
    except Exception:
        db.close()
        partial.unlink(missing_ok=True)
        raise
    db.close()
    partial.replace(output)
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--samples", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--allow-synthetic", action="store_true", help="test fixtures only; marked in every artifact")
    args = parser.parse_args()
    result = prepare(args.manifest, args.samples, args.output, args.allow_synthetic)
    print(json.dumps({"output": str(args.output), "source": result["source"], "counts": result["counts"]}, indent=2))


if __name__ == "__main__":
    main()
