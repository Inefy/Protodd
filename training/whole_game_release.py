"""Resumable, verified Protoss whole-game replay shards from the frozen v2 split.

Only qualified train and validation perspectives are eligible. Validation shards
are for selection/measurement, never optimizer batches. The sealed test split is
not read. Candidate command labels still do not prove eventual completion.
"""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ProcessPoolExecutor, as_completed
import gzip
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
from uuid import uuid4

from .replay_assets import verify
from .whole_game_labels import materialize
from .whole_game_pilot import SCHEMA as EXTRACTOR_SCHEMA, analyse, sha256

SCHEMA = "protodd-whole-game-release-v1"
MIN_FREE_BYTES = 80_000_000_000
MATCHUPS = ("PvT", "PvZ", "PvP")
SPLITS = ("train", "validation")
ARTIFACTS = ("summary.json", "terrain.json", "observations.jsonl",
             "commands.jsonl", "checkpoints.jsonl", "imitation-labels.jsonl")


def select_games(manifest, per_matchup=None):
    if per_matchup is not None and (type(per_matchup) is not int or per_matchup < 1):
        raise ValueError("per_matchup must be positive")
    selected, counts = [], Counter()
    for game in manifest["games"]:
        split = game["split"]
        if split not in SPLITS:
            continue  # Never open final-test replay files.
        for perspective, (race, quality) in enumerate(zip(game["races"], game["player_quality"])):
            if race != "P" or quality != "qualified_ladder":
                continue
            matchup = "Pv" + game["races"][1 - perspective]
            if matchup not in MATCHUPS or (per_matchup is not None and counts[split, matchup] >= per_matchup):
                continue
            selected.append(dict(game_id=game["game_id"], path=game["path"],
                                 replay_sha256=game["replay_sha256"], split=split,
                                 matchup=matchup, perspective=perspective,
                                 valid_through_frame=game["valid_through_frame"]))
            counts[split, matchup] += 1
            break
    if not selected or (per_matchup is not None and any(counts[s, m] != per_matchup for s in SPLITS for m in MATCHUPS)):
        raise ValueError("insufficient qualified games for requested split/matchup sample")
    if len({r["game_id"] for r in selected}) != len(selected):
        raise ValueError("duplicate selected game")
    return selected


def key(record):
    digest = record["game_id"].removeprefix("game:")
    if len(digest) != 64 or set(digest) - set("0123456789abcdef"):
        raise ValueError("invalid game id")
    return f"{record['split']}/{record['matchup']}/{digest}"


def atomic_json(path, data):
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + "." + uuid4().hex + ".tmp")
    temporary.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    os.replace(temporary, path)


def check_inputs(identity):
    for name, digest in identity["input_sha256"].items():
        if sha256(name) != digest:
            raise ValueError(f"pinned input changed: {name}")


def verified_receipt(destination, record, identity_sha):
    receipt_file = destination / "receipt.json"
    if not receipt_file.exists():
        return None
    receipt = json.loads(receipt_file.read_text(encoding="utf-8"))
    if (receipt.get("schema") != SCHEMA or receipt.get("identity_sha256") != identity_sha
            or receipt.get("record") != record or set(receipt.get("artifact_sha256", {})) != set(ARTIFACTS)):
        raise ValueError(f"invalid receipt: {receipt_file}")
    for name, digest in receipt["artifact_sha256"].items():
        if sha256(destination / (name + ".gz")) != digest:
            raise ValueError(f"damaged shard: {destination / name}")
    return receipt


def run_command(name, command, work, timeout):
    with (work / f"{name}.stdout.log").open("wb") as stdout, (work / f"{name}.stderr.log").open("wb") as stderr:
        try:
            subprocess.run([str(v) for v in command], stdout=stdout, stderr=stderr,
                           timeout=timeout, check=True)
        except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as exc:
            stderr.flush()
            detail = (work / f"{name}.stderr.log").read_bytes()[-2048:].decode("utf-8", errors="replace")
            raise RuntimeError(f"{name} failed: {exc}; stderr tail: {detail}") from exc


def compare_checkpoints(extracted, reference_log):
    checkpoints = {r["frame"]: r for r in map(json.loads, (extracted / "checkpoints.jsonl").read_text().splitlines())}
    compared = set()
    with reference_log.open() as stream:
        for line in stream:
            row = json.loads(line)
            frame = row["frame"]
            if frame in checkpoints:
                if checkpoints[frame] != row:
                    raise ValueError(f"instrumentation changed playback at frame {frame}")
                compared.add(frame)
    expected = {f for f in checkpoints if f % 240 == 0}
    if not expected or not expected.issubset(compared):
        raise ValueError("missing reference checkpoints")
    return len(compared)


def gzip_file(source, destination):
    with Path(source).open("rb") as input_file, Path(destination).open("wb") as output_file:
        with gzip.GzipFile(filename="", mode="wb", fileobj=output_file, compresslevel=6, mtime=0) as compressed:
            shutil.copyfileobj(input_file, compressed, length=1024 * 1024)


def remove_uncommitted(destination, games_root):
    destination, games_root = destination.resolve(), games_root.resolve()
    if not destination.is_relative_to(games_root) or destination == games_root:
        raise ValueError("uncommitted shard path escaped games directory")
    if (destination / "receipt.json").exists():
        raise ValueError(f"existing shard receipt needs verification: {destination}")
    if destination.exists():
        shutil.rmtree(destination)


def commit_shard(staged, destination, receipt, games_root):
    """Commit a directory, with a receipt-last copy fallback for Windows locks."""
    for attempt in range(8):
        try:
            os.replace(staged, destination)
            return
        except PermissionError:
            if destination.exists():
                break
            time.sleep(0.15 * (attempt + 1))
    # Some Windows file filters temporarily prevent directory renames. Copying
    # into a new directory keeps incomplete shards invisible to the reader;
    # receipt.json is still committed last.
    destination.mkdir(exist_ok=False)
    try:
        for name, digest in receipt["artifact_sha256"].items():
            target = destination / (name + ".gz")
            shutil.copyfile(staged / (name + ".gz"), target)
            if sha256(target) != digest:
                raise ValueError(f"copied shard hash mismatch: {name}")
        atomic_json(destination / "receipt.json", receipt)
    except Exception:
        remove_uncommitted(destination, games_root)
        raise


def process_game(record, config, identity_sha):
    started = time.monotonic()
    output = Path(config["output"])
    games_root = output / "games"
    destination = games_root / key(record)
    if destination.exists():
        receipt = verified_receipt(destination, record, identity_sha)
        if receipt is None:
            remove_uncommitted(destination, games_root)
        else:
            return receipt
    if shutil.disk_usage(output).free < MIN_FREE_BYTES:
        raise RuntimeError("less than 80 GB free; extraction stopped before opening another replay")
    scratch_root = (output / ".scratch").resolve()
    scratch_root.mkdir(parents=True, exist_ok=True)
    work = (scratch_root / uuid4().hex).resolve()
    if not work.is_relative_to(scratch_root) or work == scratch_root:
        raise ValueError("scratch path escaped release")
    work.mkdir()
    try:
        replay_root = Path(config["root"]).resolve()
        replay = (replay_root / record["path"]).resolve()
        if not replay.is_relative_to(replay_root) or sha256(replay) != record["replay_sha256"]:
            raise ValueError("replay path/hash mismatch")
        raw = work / "replay.raw"
        extracted = work / "extracted"
        prefix = record["valid_through_frame"]
        if config["max_frames"] is not None:
            prefix = min(prefix, config["max_frames"])
        run_command("decode", [config["decoder"], replay, raw], work, config["timeout"])
        run_command("extract", [config["extractor"], config["mpq"], config["assets"], raw,
                                extracted, prefix, record["perspective"]], work, config["timeout"])
        run_command("reference", [config["reference"], config["mpq"], config["assets"], raw, 240],
                    work, config["timeout"])
        report = analyse(extracted)
        report["labels"] = materialize(extracted, work / "imitation-labels.jsonl")
        report["checkpoints_matched"] = compare_checkpoints(extracted, work / "reference.stdout.log")
        report["valid_through_frame"] = prefix
        report["elapsed_seconds"] = round(time.monotonic() - started, 2)
        staged = work / "shard"
        staged.mkdir()
        source = {name: (work if name == "imitation-labels.jsonl" else extracted) / name for name in ARTIFACTS}
        hashes, sizes = {}, {}
        for name, path in source.items():
            target = staged / (name + ".gz")
            gzip_file(path, target)
            hashes[name] = sha256(target)
            sizes[name] = dict(raw=path.stat().st_size, compressed=target.stat().st_size)
        receipt = dict(schema=SCHEMA, extractor_schema=EXTRACTOR_SCHEMA,
                       identity_sha256=identity_sha, record=record, report=report,
                       artifact_sha256=hashes, artifact_bytes=sizes,
                       command_completion_verified=False, training_ready=False)
        atomic_json(staged / "receipt.json", receipt)
        destination.parent.mkdir(parents=True, exist_ok=True)
        commit_shard(staged, destination, receipt, games_root)
        return receipt
    finally:
        # Only remove our own resolved child of the release scratch directory.
        if work.is_relative_to(scratch_root) and work != scratch_root and work.exists():
            shutil.rmtree(work)


def run(args):
    source_release = json.loads((args.release / "release.json").read_text())
    manifest_path = args.release / "manifest.json"
    if not source_release.get("complete") or sha256(manifest_path) != source_release["manifest_sha256"]:
        raise ValueError("frozen data release changed or incomplete")
    verify(args.assets)
    selected = select_games(json.loads(manifest_path.read_text()), args.per_matchup)
    root = Path(__file__).resolve().parents[1]
    source_files = [Path(__file__), root / "training/whole_game_pilot.py", root / "training/whole_game_labels.py",
                    root / "training/whole_game_actor_dedupe.py",
                    root / "training/replay_assets.py", root / "include/protodd/WholeGameObservation.hpp",
                    root / "tools/replay_native/CMakeLists.txt",
                    *sorted((root / "tools/replay_native").glob("*.hpp")),
                    *sorted((root / "tools/replay_native").glob("*.cpp"))]
    paths = [manifest_path, *source_files, args.extractor, args.selftest, args.decoder, args.reference,
             args.assets / "manifest.json",
             *(args.mpq / name for name in ("StarDat.mpq", "BrooDat.mpq", "Patch_rt.mpq"))]
    identity = dict(schema=SCHEMA, extractor_schema=EXTRACTOR_SCHEMA,
                    source_release_manifest_sha256=sha256(manifest_path),
                    per_matchup=args.per_matchup, max_frames=args.max_frames,
                    selected=selected, input_sha256={str(p.resolve()): sha256(p) for p in paths})
    identity_bytes = (json.dumps(identity, sort_keys=True, separators=(",", ":")) + "\n").encode()
    identity_sha = hashlib.sha256(identity_bytes).hexdigest()
    args.output.mkdir(parents=True, exist_ok=True)
    identity_file = args.output / "identity.json"
    if identity_file.exists():
        if json.loads(identity_file.read_text()) != identity:
            raise ValueError("existing release identity differs")
    elif any(args.output.iterdir()):
        raise ValueError("output exists without identity")
    else:
        atomic_json(identity_file, identity)
        for source in source_files:
            target = args.output / "source" / source.resolve().relative_to(root)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, target)
    check_inputs(identity)
    selftest_receipt = args.output / "selftest.json"
    if selftest_receipt.exists():
        record = json.loads(selftest_receipt.read_text())
        if record.get("identity_sha256") != identity_sha or record.get("passed") is not True:
            raise ValueError("self-test receipt does not match release")
    else:
        fixture = selected[0]
        replay_root = args.root.resolve()
        replay = (replay_root / fixture["path"]).resolve()
        if not replay.is_relative_to(replay_root) or sha256(replay) != fixture["replay_sha256"]:
            raise ValueError("self-test fixture replay path/hash mismatch")
        scratch_root = (args.output / ".scratch").resolve()
        scratch_root.mkdir(parents=True, exist_ok=True)
        work = (scratch_root / uuid4().hex).resolve()
        if not work.is_relative_to(scratch_root) or work == scratch_root:
            raise ValueError("self-test scratch escaped release")
        work.mkdir()
        try:
            raw = work / "replay.raw"
            run_command("decode", [args.decoder, replay, raw], work, args.timeout)
            run_command("selftest", [args.selftest, args.mpq, args.assets, raw, fixture["perspective"]],
                        work, args.timeout)
            atomic_json(selftest_receipt, dict(identity_sha256=identity_sha, passed=True,
                        fixture_game=fixture["game_id"],
                        stdout_sha256=sha256(work / "selftest.stdout.log")))
        finally:
            if work.is_relative_to(scratch_root) and work != scratch_root and work.exists():
                shutil.rmtree(work)
    if args.initialize_only:
        return dict(schema=SCHEMA, initialized=True, identity_sha256=identity_sha,
                    games=len(selected))
    pending, reports = [], []
    for record in selected:
        destination = args.output / "games" / key(record)
        if destination.exists():
            receipt = verified_receipt(destination, record, identity_sha)
            if receipt is None:
                remove_uncommitted(destination, args.output / "games")
                pending.append(record)
            else:
                reports.append(receipt)
        else:
            pending.append(record)
    config = dict(output=str(args.output.resolve()), root=str(args.root.resolve()),
                  decoder=str(args.decoder.resolve()), extractor=str(args.extractor.resolve()),
                  reference=str(args.reference.resolve()), mpq=str(args.mpq.resolve()),
                  assets=str(args.assets.resolve()), max_frames=args.max_frames, timeout=args.timeout)
    failures = []
    compressed_bytes = sum(sum(v["compressed"] for v in r["artifact_bytes"].values()) for r in reports)
    atomic_json(args.output / "progress.json", dict(schema=SCHEMA, completed=len(reports),
                total=len(selected), failures=failures, compressed_bytes=compressed_bytes))
    with ProcessPoolExecutor(max_workers=args.workers) as pool:
        futures = {pool.submit(process_game, record, config, identity_sha): record for record in pending}
        for future in as_completed(futures):
            record = futures[future]
            try:
                receipt = future.result()
                reports.append(receipt)
                compressed_bytes += sum(v["compressed"] for v in receipt["artifact_bytes"].values())
                print(json.dumps(dict(done=key(record), seconds=receipt["report"]["elapsed_seconds"],
                                      completed=len(reports), total=len(selected))), flush=True)
            except Exception as exc:
                failures.append(dict(game=key(record), error=str(exc)))
                print(json.dumps(failures[-1]), flush=True)
            atomic_json(args.output / "progress.json", dict(schema=SCHEMA,
                        completed=len(reports), total=len(selected), failures=failures,
                        compressed_bytes=compressed_bytes))
    check_inputs(identity)
    if failures:
        raise RuntimeError(f"{len(failures)} game extraction failures; rerun after diagnosis")
    if len(reports) != len(selected):
        raise ValueError("missing completed shards")
    by_split = Counter(r["record"]["split"] for r in reports)
    by_matchup = Counter((r["record"]["split"], r["record"]["matchup"]) for r in reports)
    result = dict(schema=SCHEMA, complete=True, training_ready=False,
                  command_completion_verified=False, identity_sha256=identity_sha,
                  games=len(reports), split_games=by_split,
                  split_matchup_games={f"{s}-{m}": n for (s, m), n in by_matchup.items()},
                  total_compressed_bytes=compressed_bytes)
    atomic_json(args.output / "release.json", result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "root", "extractor", "selftest", "decoder", "reference", "mpq", "assets", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--per-matchup", type=int, default=None,
                        help="Bounded train and validation sample per matchup; omit for full split")
    parser.add_argument("--max-frames", type=int, default=None,
                        help="Bounded benchmark prefix; omit for full valid prefix")
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--timeout", type=int, default=1800)
    parser.add_argument("--initialize-only", action="store_true",
                        help="Write a pinned identity and self-test before migrating verified shards")
    args = parser.parse_args()
    if (args.workers < 1 or args.timeout < 1 or
            (args.max_frames is not None and args.max_frames < 1)):
        parser.error("workers, timeout and frame cap must be positive")
    print(json.dumps(run(args), default=dict, indent=2))


if __name__ == "__main__":
    main()
