"""Prepare checked human replay observations, then launch CUDA after the full audit finishes."""
from collections import Counter
from datetime import datetime, timezone
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import shutil
import sqlite3
import subprocess
import tempfile
import time
from types import SimpleNamespace

from training.playback_check import ROOT, run_command
from training.prepare import prepare, validate_manifest, validate_sample
from training.replay_assets import verify
from training.replay_cohort import freeze
from training.schema import load_schema, sha256


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def checkpoints_hash(path):
    result = hashlib.sha256()
    with Path(path).open() as stream:
        for line in stream:
            result.update(json.dumps(json.loads(line), separators=(",", ":")).encode())
    return result.hexdigest()


def extract_game(game, validation, config, schema):
    output = config.output / "games" / game["replay_sha256"]
    # An interrupted attempt may leave compressed partial files. Recreate only
    # this game's generated outputs; complete, hashed results are reused by main.
    output.mkdir(parents=True, exist_ok=True)
    try:
        with tempfile.TemporaryDirectory(prefix="extract-", dir=config.output) as directory:
            temp = Path(directory)
            source = config.root / game["path"]
            copied = temp / "input.rep"
            shutil.copyfile(source, copied)
            if sha256(copied) != game["replay_sha256"]:
                raise ValueError("Replay changed after audit")
            raw = temp / "replay.raw"
            conversion = json.loads(run_command([str(config.decoder), str(copied), str(raw)], config.timeout))
            if conversion["decoded_sha256"] != validation["conversion"]["decoded_sha256"]:
                raise ValueError("Decoded bytes differ from playback validation")
            samples, actions, checkpoints = [temp / name for name in ("samples.jsonl", "actions.jsonl", "checkpoints.jsonl")]
            stats = json.loads(run_command([str(config.extractor), str(config.mpq), str(config.assets), str(raw),
                game["game_id"], str(samples), str(actions), str(checkpoints)], config.timeout))
            if stats["schema"] != schema["version"] or stats["fingerprint"] != schema["fingerprint"]:
                raise ValueError("Extractor schema differs from trainer")
            if stats["end_frame"] != game["valid_through_frame"]:
                raise ValueError("Extractor stopped at wrong frame")
            checkpoint_digest = checkpoints_hash(checkpoints)
            if checkpoint_digest != validation["native_checkpoints_sha256"]:
                raise ValueError("Instrumented action execution changed playback checkpoints")
            for perspective in stats["perspectives"]:
                if game["slots"][perspective["perspective"]] != perspective["slot"]:
                    raise ValueError("Extractor/audit player slots differ")
            counts = Counter()
            previous = {}
            # Validate the same data contract used by the final importer.
            with samples.open() as source, gzip.open(output / "samples.jsonl.gz", "wt", encoding="utf-8") as target:
                for line in source:
                    row = json.loads(line)
                    if game["player_quality"][row["perspective"]] == "unknown":
                        continue
                    validate_sample(row, {game["game_id"]: game}, schema)
                    if row["frame"] <= previous.get(row["perspective"], -1) or row["action_frame"] - row["frame"] >= 24:
                        raise ValueError("Noncausal or duplicate extraction window")
                    previous[row["perspective"]] = row["frame"]
                    counts[row["action"]] += 1
                    target.write(line)
            if sum(counts.values()) < 50 or sum(v for k, v in counts.items() if k != "wait") < 5:
                raise ValueError("Too few usable observations/accepted macro actions")
            with actions.open("rb") as source, gzip.open(output / "actions.jsonl.gz", "wb") as target:
                shutil.copyfileobj(source, target)
            record = {"status": "extracted", "game_id": game["game_id"], "samples": sum(counts.values()),
                "counts": dict(counts), "stats": stats, "checkpoints_sha256": checkpoint_digest,
                "samples_sha256": sha256(output / "samples.jsonl.gz"),
                "actions_sha256": sha256(output / "actions.jsonl.gz"),
                "authoritative_game_validated": False}
    except (ValueError, KeyError, OSError, subprocess.TimeoutExpired) as error:
        record = {"status": "quarantined", "error": str(error), "game_id": game["game_id"]}
    write_json(output / "result.json", record)
    return record


def validate_launch(validation_summary, games, results):
    if not validation_summary.get("complete") or validation_summary["processed"] != validation_summary["total"]:
        raise ValueError("Full playback validation has not completed")
    if validation_summary["matched"] / validation_summary["total"] < .95:
        raise ValueError("More than 5% of corpus failed playback; investigate before training")
    usable = [g for g in games if results.get(g["game_id"], {}).get("status") == "extracted"]
    if len(usable) < .95 * len(games):
        raise ValueError("More than 5% of qualified cohort failed extraction/playback")
    counts = Counter(g["split"] for g in usable)
    if counts["train"] < 100 or counts["validation"] < 100 or counts["test"] < 100:
        raise ValueError("At least 100 usable games per split are required")
    held_out = {p for g in games for p in g["player_keys"] if int(p[:8], 16) % 100 < 5}
    for game in usable:
        if game["split"] == "train" and (game["map_holdout"] or held_out.intersection(game["player_keys"])):
            raise ValueError("A held-out map/player entered training")
    return usable


def main():
    # Retain this version's extraction functions for verified artifact reuse.
    # The old all-in-one entry point must never auto-launch a training job.
    raise SystemExit("Legacy automatic training launcher retired. Use python -m training.data_release "
                     "--config CONFIG --output RELEASE --legacy-sources SNAPSHOT for extraction only; "
                     "training must be launched separately with a reviewed experiment config.")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    raw_config = json.loads(args.config.read_text())
    config = SimpleNamespace(**raw_config)
    for name in ("root", "audit", "validation", "extractor", "decoder", "mpq", "assets", "output", "python", "model_tool", "evidence"):
        setattr(config, name, Path(getattr(config, name)).resolve())
    config.timeout = raw_config.get("timeout", 300)
    config.output.mkdir(parents=True, exist_ok=True)
    (config.output / "games").mkdir(exist_ok=True)
    schema = load_schema()
    identity = {"config": raw_config, "schema": schema, "terrain": verify(config.assets), "hashes": {}}
    source_files = [*sorted((ROOT / "training").glob("*.py")), ROOT / "training/schema_v2.json",
                    config.extractor, config.decoder, config.model_tool, config.evidence,
                    config.validation / "identity.json", config.audit,
                    ROOT / "training/requirements.txt", config.python]
    for path in source_files:
        identity["hashes"][str(path)] = sha256(path)
    evidence = json.loads(config.evidence.read_text())
    if (not evidence.get("passed") or evidence.get("extractor_sha256") != sha256(config.extractor)
            or evidence.get("schema") != schema["version"] or evidence.get("fingerprint") != schema["fingerprint"]):
        raise ValueError("Current extractor has not passed its perspective/action integration audit")
    pin = config.output / "identity.json"
    if pin.exists() and json.loads(pin.read_text()) != identity:
        raise ValueError("Pipeline inputs changed; use a new output directory")
    write_json(pin, identity)
    with sqlite3.connect(config.audit) as db:
        records = [json.loads(r[0]) for r in db.execute("SELECT detail FROM replays")]
    cohort_file = config.output / "cohort.json"
    cohort = freeze(records)
    if cohort_file.exists() and json.loads(cohort_file.read_text()) != cohort:
        raise ValueError("Frozen cohort changed")
    write_json(cohort_file, cohort)
    games = cohort["games"]
    results = {}
    for game in games:
        result_file = config.output / "games" / game["replay_sha256"] / "result.json"
        if result_file.exists():
            result = json.loads(result_file.read_text())
            if result["status"] == "extracted" and sha256(result_file.parent / "samples.jsonl.gz") != result["samples_sha256"]:
                raise ValueError("Stored extraction changed")
            results[game["game_id"]] = result
    def status(phase, **extra):
        counts = Counter(r["status"] for r in results.values())
        write_json(config.output / "status.json", {"phase": phase, "updated_at": datetime.now(timezone.utc).isoformat(),
            "cohort_games": len(games), "extracted_games": counts["extracted"], "quarantined_games": counts["quarantined"],
            "samples": sum(r.get("samples", 0) for r in results.values()),
            "training_started": phase in ("training", "verifying_export", "complete"), **extra})
    while len(results) < len(games):
        status("validating_and_extracting")
        with sqlite3.connect(config.validation / "validation.sqlite", timeout=30) as db:
            checked = {path: json.loads(detail) for path, detail in db.execute("SELECT path,detail FROM results")}
        progress = False
        for game in games:
            if game["game_id"] in results or game["path"] not in checked:
                continue
            validation = checked[game["path"]]
            if validation["status"] != "checkpoints_matched" or validation.get("sha256") != game["replay_sha256"]:
                result = {"status": "quarantined", "error": "Playback validation did not pass"}
            else:
                result = extract_game(game, validation, config, schema)
            results[game["game_id"]] = result
            print(json.dumps({"game": game["game_id"], "status": result["status"], "samples": result.get("samples"),
                              "error": result.get("error")}), flush=True)
            status("validating_and_extracting")
            progress = True
        summary = json.loads((config.validation / "summary.json").read_text())
        if summary.get("complete") and not progress:
            break
        if not progress:
            time.sleep(30)
    # Even if all qualified games finish early, every downloaded Protoss replay
    # must be audited before the user-authorized CUDA phase may begin.
    while True:
        validation_summary = json.loads((config.validation / "summary.json").read_text())
        if validation_summary.get("complete"):
            break
        status("waiting_for_full_validation")
        time.sleep(30)
    for path, expected in identity["hashes"].items():
        if sha256(path) != expected:
            raise ValueError(f"Pinned pipeline input changed before training: {path}")
    usable = validate_launch(validation_summary, games, results)
    manifest = {"schema": schema["version"], "fingerprint": schema["fingerprint"], "source": "human_replay",
        "extractor": "native-protoss-v2", "audit_id": "full-corpus-cross-engine-and-perspective-v2",
        "validation": {"playback": True, "perspective": True, "actions": True},
        "playback_validation_level": "cross_engine_checkpoints_240_and_instrumented_state_parity",
        "authoritative_game_validated": False, "deployment": "shadow-only", "games": usable,
        "label_contract": "first accepted mapped macro request in [frame,frame+24); repeated builds excluded; otherwise wait",
        "validation_evidence_sha256": sha256(config.evidence), "cohort_sha256": sha256(cohort_file)}
    validate_manifest(manifest, schema)
    write_json(config.output / "manifest.json", manifest)
    status("preparing_dataset", usable_games=len(usable))
    merged = config.output / "samples.jsonl.gz"
    if not merged.exists():
        with gzip.open(merged.with_suffix(".partial"), "wb") as target:
            for game in usable:
                with gzip.open(config.output / "games" / game["replay_sha256"] / "samples.jsonl.gz", "rb") as source:
                    shutil.copyfileobj(source, target)
        merged.with_suffix(".partial").replace(merged)
    dataset = config.output / "dataset.sqlite"
    if not dataset.exists():
        prepare(config.output / "manifest.json", merged, dataset)
    status("training", usable_games=len(usable))
    with (config.output / "training.stdout.log").open("ab") as out, (config.output / "training.stderr.log").open("ab") as err:
        subprocess.run([str(config.python), "-u", "-m", "training.train", "--dataset", str(dataset),
            "--output", str(config.output / "model"), "--device", "cuda", "--epochs", "20", "--batch-size", "512"],
            cwd=ROOT, stdout=out, stderr=err, check=True)
        status("verifying_export", usable_games=len(usable))
        subprocess.run([str(config.python), "-m", "training.verify_export", "--dataset", str(dataset),
            "--model", str(config.output / "model/LearnedMacro.bin"), "--tool", str(config.model_tool),
            "--output", str(config.output / "model/export-parity.json")],
            cwd=ROOT, stdout=out, stderr=err, check=True)
    status("complete", model=str(config.output / "model/LearnedMacro.bin"), final_test_evaluated=False,
           strength_validated=False, authoritative_game_validated=False)


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Retain an actionable status if a background run stops at a gate.
        import sys
        try:
            cfg = json.loads(Path(sys.argv[sys.argv.index("--config") + 1]).read_text())
            path = Path(cfg["output"]) / "status.json"
            previous = json.loads(path.read_text()) if path.exists() else {}
            write_json(path, {**previous, "phase": "failed", "error": str(error),
                             "updated_at": datetime.now(timezone.utc).isoformat()})
        except Exception:
            pass
        raise
