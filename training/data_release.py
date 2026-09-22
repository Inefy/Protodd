"""Resume pinned v2 extraction and publish an immutable data release. Never trains."""
from __future__ import annotations

import argparse
import ast
from collections import Counter
from contextlib import closing, contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import time
from types import SimpleNamespace

from .playback_check import ROOT
from .prepare import validate_manifest
from .replay_assets import verify
from .replay_cohort import freeze
from .replay_pipeline import extract_game, validate_launch, write_json
from .schema import load_schema

EXTRACTION_SOURCES = {"__init__.py", "replay_pipeline.py", "playback_check.py", "prepare.py",
                      "replay_assets.py", "replay_cohort.py", "schema.py", "schema_v2.json"}


def digest(path):
    result = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            result.update(block)
    return result.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8-sig"))


def immutable_json(path, value):
    path = Path(path)
    if path.exists():
        if read_json(path) != value:
            raise ValueError(f"Immutable release artifact changed: {path}")
        return
    write_json(path, value)


def extraction_code(source):
    """Only a legacy CLI-body change is compatible; helpers/imports remain pinned."""
    tree = ast.parse(source)
    tree.body = [node for node in tree.body if not (isinstance(node, ast.FunctionDef) and node.name == "main")]
    return ast.dump(tree, include_attributes=False)


def verify_pins(identity, snapshot, root=ROOT):
    """Separate extraction dependencies from historical trainer-only dependencies."""
    root, snapshot = Path(root).resolve(), Path(snapshot).resolve()
    snapshot_manifest = read_json(snapshot / "snapshot.json")
    checked, historical = {}, {}
    training_only = {(root / identity["config"][key]).resolve() for key in ("python", "model_tool")}
    for name, expected in identity["hashes"].items():
        path = Path(name)
        is_source = path.parent == root / "training"
        if is_source:
            archived = snapshot / "training" / path.name
            if snapshot_manifest["files"].get(path.name) != expected or digest(archived) != expected:
                raise ValueError(f"Archived pinned source changed: {archived}")
        if (is_source and path.name not in EXTRACTION_SOURCES) or path in training_only:
            historical[name] = expected
            continue
        actual = digest(path)
        if actual != expected:
            if not (is_source and path.name == "replay_pipeline.py" and
                    extraction_code(path.read_text(encoding="utf-8")) ==
                    extraction_code(archived.read_text(encoding="utf-8"))):
                raise ValueError(f"Pinned extraction input changed: {path}")
        checked[name] = actual
    required = {str(root / "training" / name) for name in EXTRACTION_SOURCES}
    if not required.issubset(checked):
        raise ValueError("Legacy identity omits required extraction sources")
    return {"extraction_hashes": checked, "historical_training_hashes": historical}


@contextmanager
def release_lock(path):
    """Kernel lock releases even if a worker is killed; the file may remain."""
    with Path(path).open("a+b") as stream:
        stream.seek(0, os.SEEK_END)
        if stream.tell() == 0:
            stream.write(b"0"); stream.flush()
        stream.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(stream.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(stream.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError("Another extraction release worker owns this output") from error
        try:
            yield
        finally:
            stream.seek(0)
            if os.name == "nt":
                msvcrt.locking(stream.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(stream.fileno(), fcntl.LOCK_UN)


def validation_records(config):
    with closing(sqlite3.connect((config.validation / "validation.sqlite").as_uri() + "?mode=ro", uri=True)) as db:
        records = {}
        for path, expected, status, detail in db.execute("SELECT path,sha256,status,detail FROM results"):
            record = json.loads(detail)
            if status not in ("checkpoints_matched", "quarantined") or record.get("status") != status:
                raise ValueError("Validation row/status mismatch")
            if record.get("sha256") != expected:
                # A failed read can quarantine a replay without recovering its hash.
                if status != "quarantined":
                    raise ValueError("Matched validation identity mismatch")
            records[path] = {**record, "audit_sha256": expected}
        return records


def verify_cached(game, record, directory, validation):
    if record.get("status") != "extracted":
        if record.get("status") != "quarantined":
            raise ValueError("Unknown extraction status")
        if record.get("game_id", game["game_id"]) != game["game_id"]:
            raise ValueError("Quarantine game identity mismatch")
        return
    if record.get("game_id") != game["game_id"]:
        raise ValueError("Cached extraction game identity mismatch")
    if (validation.get("status") != "checkpoints_matched" or
            validation.get("sha256") != game["replay_sha256"] or
            record.get("checkpoints_sha256") != validation.get("native_checkpoints_sha256") or
            not record.get("checkpoints_sha256")):
        raise ValueError("Cached extraction lost its matched playback evidence")
    if (type(record.get("samples")) is not int or record["samples"] < 50 or
            sum(record.get("counts", {}).values()) != record["samples"]):
        raise ValueError("Invalid cached extraction counts")
    if (record.get("stats", {}).get("schema") != load_schema()["version"] or
            record.get("stats", {}).get("fingerprint") != load_schema()["fingerprint"] or
            record.get("stats", {}).get("end_frame") != game["valid_through_frame"]):
        raise ValueError("Cached extraction schema/prefix mismatch")
    for filename, key in (("samples.jsonl.gz", "samples_sha256"), ("actions.jsonl.gz", "actions_sha256")):
        if digest(directory / filename) != record.get(key):
            raise ValueError(f"Stored extraction changed: {directory / filename}")


def final_validation(summary, records, expected_tasks):
    if not summary.get("complete"):
        raise ValueError("Full playback validation has not completed")
    if set(records) != set(expected_tasks):
        raise ValueError("Validation results differ from the frozen audit")
    if any(records[path]["audit_sha256"] != expected for path, expected in expected_tasks.items()):
        raise ValueError("Validated replay differs from the frozen audit")
    counts = Counter(record["status"] for record in records.values())
    expected = {"total": len(expected_tasks), "processed": len(records), "pending": 0,
                "matched": counts["checkpoints_matched"], "quarantined": counts["quarantined"]}
    if any(summary.get(key) != value for key, value in expected.items()):
        raise ValueError("Validation summary does not agree with its results")


def run(config_path, output, legacy_sources, follow=False, poll_seconds=30):
    if not 1 <= poll_seconds <= 60:
        raise ValueError("poll_seconds must be between 1 and 60")
    raw = read_json(config_path)
    config = SimpleNamespace(**raw)
    for key in ("root", "audit", "validation", "extractor", "decoder", "mpq", "assets", "output", "evidence"):
        setattr(config, key, (ROOT / getattr(config, key)).resolve())
    config.timeout = raw.get("timeout", 300)
    output, snapshot = Path(output).resolve(), Path(legacy_sources).resolve()
    if output == config.output or output in config.output.parents:
        raise ValueError("Use a new release directory, never replace the original run")
    output.mkdir(parents=True, exist_ok=True)
    # Different release destinations still share the legacy games/status paths.
    # Take their common source lock first, then the destination lock.
    with release_lock(config.output / "extraction-release.lock"):
        with release_lock(output / "release.lock"):
            return _run(config, raw, output, snapshot, follow, poll_seconds)


def _run(config, raw, output, snapshot, follow, poll_seconds):
    old_identity_path = config.output / "identity.json"
    old_identity = read_json(old_identity_path)
    if old_identity["config"] != raw:
        raise ValueError("Legacy config differs from the extraction identity")
    if read_json(snapshot / "snapshot.json")["legacy_identity_sha256"] != digest(old_identity_path):
        raise ValueError("Source snapshot belongs to another legacy run")
    pins = verify_pins(old_identity, snapshot)
    schema = load_schema()
    if old_identity["schema"] != schema or verify(config.assets) != old_identity["terrain"]:
        raise ValueError("Pinned extraction schema or terrain changed")
    evidence = read_json(config.evidence)
    if (evidence.get("passed") is not True or evidence.get("extractor_sha256") != digest(config.extractor)
            or evidence.get("schema") != schema["version"] or evidence.get("fingerprint") != schema["fingerprint"]):
        raise ValueError("Extractor evidence does not match this release")
    validation_identity = read_json(config.validation / "identity.json")
    for name, expected in validation_identity["hashes"].items():
        if digest(name) != expected:
            raise ValueError(f"Pinned validation input changed: {name}")
    with closing(sqlite3.connect(config.audit.as_uri() + "?mode=ro", uri=True)) as db:
        rows = db.execute("SELECT path,sha256,detail FROM replays").fetchall()
    expected_tasks = {path: hashed for path, hashed, _ in rows}
    cohort_path = config.output / "cohort.json"
    cohort = read_json(cohort_path)
    if cohort != freeze([json.loads(detail) for _, _, detail in rows]):
        raise ValueError("Frozen cohort differs from its audit")
    identity = {"format_version": 1, "stage": "extraction_release", "automatic_training": False,
                "legacy_run": str(config.output), "legacy_identity_sha256": digest(old_identity_path),
                "legacy_sources": str(snapshot), "cohort_sha256": digest(cohort_path),
                "schema": schema, "orchestrator_sha256": digest(__file__), **pins}
    identity_path = output / "identity.json"
    if identity_path.exists() and read_json(identity_path) != identity:
        raise ValueError("Release identity changed; use a new release output")
    if not identity_path.exists():
        write_json(identity_path, identity)
    games, results = cohort["games"], {}
    last_status = 0.0

    def status(phase, force=False, **extra):
        nonlocal last_status
        if not force and time.monotonic() - last_status < 15:
            return
        counts = Counter(r["status"] for r in results.values())
        value = {"phase": phase, "updated_at": datetime.now(timezone.utc).isoformat(),
                 "pid": os.getpid(), "release": str(output), "cohort_games": len(games),
                 "extracted_games": counts["extracted"], "quarantined_games": counts["quarantined"],
                 "unresolved_games": len(games) - len(results), "checked_cached_games": len(results),
                 "samples": sum(r.get("samples", 0) for r in results.values()),
                 "training_started": False, "automatic_training_held": True,
                 "extraction_held": False, **extra}
        write_json(output / "status.json", value)
        # Do not replace known extraction totals with a partial cache-check count.
        if phase != "checking_cached_extractions":
            write_json(config.output / "status.json", value)
        print(json.dumps(value), flush=True)
        last_status = time.monotonic()

    checked = validation_records(config)
    status("checking_cached_extractions", force=True)
    for game in games:
        directory = config.output / "games" / game["replay_sha256"]
        result_path = directory / "result.json"
        if result_path.exists():
            record = read_json(result_path)
            validation = checked.get(game["path"], {})
            verify_cached(game, record, directory, validation)
            results[game["game_id"]] = record
        status("checking_cached_extractions")

    while True:
        checked = validation_records(config)
        for game in games:
            if game["game_id"] in results or game["path"] not in checked:
                continue
            validation = checked[game["path"]]
            if validation["audit_sha256"] != game["replay_sha256"]:
                raise ValueError("Replay validation identity changed")
            if validation["status"] != "checkpoints_matched":
                record = {"status": "quarantined", "game_id": game["game_id"],
                          "error": "Playback validation did not pass"}
                directory = config.output / "games" / game["replay_sha256"]
                directory.mkdir(parents=True, exist_ok=True)
                write_json(directory / "result.json", record)
            else:
                status("extracting", force=True, current_game=game["game_id"])
                record = extract_game(game, validation, config, schema)
                verify_cached(game, record, config.output / "games" / game["replay_sha256"], validation)
            results[game["game_id"]] = record
            status("extracting")
        summary = read_json(config.validation / "summary.json")
        if summary.get("complete"):
            # The validator may have finished while we were extracting a game.
            checked = validation_records(config)
            final_validation(summary, checked, expected_tasks)
            if len(results) != len(games):
                if any(g["path"] not in checked for g in games):
                    raise ValueError("Completed validation left unresolved cohort games")
                continue
            break
        status("waiting_for_full_validation", force=True, validation_pending=summary.get("pending"))
        if not follow:
            return None
        time.sleep(poll_seconds)

    # Recheck extraction inputs before publishing, but trainer evolution cannot
    # invalidate a release: historical trainer hashes are provenance only.
    if verify_pins(old_identity, snapshot) != pins or digest(__file__) != identity["orchestrator_sha256"]:
        raise ValueError("Extraction implementation changed during release")
    if digest(cohort_path) != identity["cohort_sha256"]:
        raise ValueError("Frozen cohort changed during release")
    usable = validate_launch(summary, games, results)
    manifest = {"schema": schema["version"], "fingerprint": schema["fingerprint"], "source": "human_replay",
                "extractor": "native-protoss-v2", "audit_id": "full-corpus-cross-engine-and-perspective-v2",
                "validation": {"playback": True, "perspective": True, "actions": True},
                "playback_validation_level": "cross_engine_checkpoints_240_and_instrumented_state_parity",
                "authoritative_game_validated": False, "deployment": "shadow-only", "games": usable,
                "label_contract": "first accepted mapped macro request in [frame,frame+24); repeated builds excluded; otherwise wait",
                "validation_evidence_sha256": digest(config.evidence), "cohort_sha256": digest(cohort_path)}
    validate_manifest(manifest, schema)
    sources = {}
    status("verifying_release", force=True)
    for game in usable:
        directory = config.output / "games" / game["replay_sha256"]
        record = results[game["game_id"]]
        if read_json(directory / "result.json") != record:
            raise ValueError("Extraction record changed before release")
        verify_cached(game, record, directory, checked[game["path"]])
        sources[game["game_id"]] = {"directory": str(directory), "result_sha256": digest(directory / "result.json"),
                                   "samples_sha256": record["samples_sha256"], "actions_sha256": record["actions_sha256"]}
        status("verifying_release")
    immutable_json(output / "manifest.json", manifest)
    immutable_json(output / "sources.json", {"format_version": 1, "games": sources})
    release = {"format_version": 1, "complete": True, "stage": "extraction_release",
               "identity_sha256": digest(identity_path), "manifest_sha256": digest(output / "manifest.json"),
               "sources_sha256": digest(output / "sources.json"), "validation_summary_sha256": digest(config.validation / "summary.json"),
               "cohort_games": len(games), "usable_games": len(usable),
               "split_games": dict(Counter(g["split"] for g in usable)),
               "samples": sum(results[g["game_id"]]["samples"] for g in usable),
               "training_started": False, "automatic_training": False,
               "tensor_preparation_required": True, "strength_validated": False,
               "authoritative_game_validated": False}
    release_path = output / "release.json"
    if release_path.exists() and read_json(release_path) != release:
        raise ValueError("Completed release changed; use a new output")
    immutable_json(release_path, release)
    status("extraction_complete_training_held", force=True, validation_pending=0, release_manifest=str(release_path))
    return release


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--legacy-sources", type=Path, required=True)
    parser.add_argument("--follow", action="store_true", help="continue extraction as validator completes; never train")
    parser.add_argument("--poll-seconds", type=int, default=30)
    args = parser.parse_args()
    try:
        run(args.config, args.output, args.legacy_sources, args.follow, args.poll_seconds)
    except Exception as error:
        if args.output.is_dir():
            # Do not overwrite another worker's status after a lock failure.
            if "owns this output" not in str(error):
                write_json(args.output / "failure.json", {"error": str(error), "training_started": False,
                           "updated_at": datetime.now(timezone.utc).isoformat()})
        raise


if __name__ == "__main__":
    main()
