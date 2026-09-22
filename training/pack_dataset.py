"""Stream extracted train/validation games into immutable bounded tensor shards.

Final-test samples and results are never opened. Each selected game's compressed
samples are verified and validated against the original importer contract.
Per-perspective disk spools keep both memory use and sequence boundaries bounded.
"""
import argparse
from collections import Counter
from datetime import datetime, timezone
import gzip
import hashlib
import json
import os
from pathlib import Path
import time

import numpy as np

from .prepare import validate_manifest, validate_sample
from .schema import load_schema, sha256
from .shards import FORMAT, METADATA_DTYPE, positive_integer


PACKER_SOURCES = ("pack_dataset.py", "shards.py", "prepare.py", "schema.py", "schema_v2.json")


def _code_hashes():
    return {name: sha256(Path(__file__).with_name(name)) for name in PACKER_SOURCES}


class _Progress:
    """Mutable sibling status, kept outside the immutable tensor release."""
    def __init__(self, output):
        self.path = output.with_name(output.name + ".status.json")
        self.started = time.monotonic()
        self.last_update = -float("inf")
        self.value = {"stage": "tensor_preparation", "pid": os.getpid(),
                      "output": str(output.resolve()), "training_started": False,
                      "samples_processed": 0, "samples_total": None,
                      "games_scanned": 0, "games_packed": 0}

    def update(self, phase=None, force=False, **extra):
        if phase is not None:
            self.value["phase"] = phase
        self.value.update(extra)
        now = time.monotonic()
        if not force and now - self.last_update < 15:
            return
        elapsed = max(0.0, now - self.started)
        processed, total = self.value["samples_processed"], self.value["samples_total"]
        remaining = None
        if self.value.get("phase") == "packing" and total and processed:
            remaining = elapsed * max(0, total - processed) / processed
        elif self.value.get("phase") == "complete":
            remaining = 0.0
        value = {**self.value, "updated_at": datetime.now(timezone.utc).isoformat(),
                 "elapsed_seconds": elapsed, "estimated_remaining_seconds": remaining}
        temporary = self.path.with_suffix(self.path.suffix + ".tmp")
        temporary.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
        temporary.replace(self.path)
        self.last_update = now


def _bind_release(manifest, manifest_digest, manifest_path, game_map, release_path):
    if manifest["source"] == "synthetic_test":
        return None, None
    path = Path(release_path) if release_path is not None else manifest_path.parent / "release.json"
    if path.is_dir():
        path = path / "release.json"
    raw = path.read_bytes()
    release = json.loads(raw)
    if (release.get("format_version") != 1 or release.get("complete") is not True or
            release.get("stage") != "extraction_release" or release.get("training_started") is not False or
            release.get("automatic_training") is not False):
        raise ValueError("complete frozen extraction release with training held is required")
    if release.get("manifest_sha256") != manifest_digest:
        raise ValueError("frozen release manifest checksum mismatch")
    sources_path = path.parent / "sources.json"
    raw_sources = sources_path.read_bytes()
    sources_digest = hashlib.sha256(raw_sources).hexdigest()
    if release.get("sources_sha256") != sources_digest:
        raise ValueError("frozen release sources checksum mismatch")
    sources = json.loads(raw_sources)
    if (sources.get("format_version") != 1 or not isinstance(sources.get("games"), dict) or
            set(sources["games"]) != set(game_map)):
        raise ValueError("frozen release source game identities mismatch")
    provenance = {"release_path": str(path.resolve()), "release_sha256": hashlib.sha256(raw).hexdigest(),
                  "sources_path": str(sources_path.resolve()), "sources_sha256": sources_digest}
    return sources["games"], provenance


def wait_for_release(manifest_path, output, release=None, poll_seconds=15):
    """CLI prerequisite wait; never repairs/replaces an existing release marker.

    Missing release.json means pending. An existing incomplete/malformed marker
    or sibling failure.json is an error. pack() still performs all content and
    provenance checks once this helper returns.
    """
    positive_integer(poll_seconds, "release poll seconds")
    if poll_seconds > 30:
        raise ValueError("release poll seconds must be at most 30")
    output = Path(output)
    partial = output.with_name(output.name + ".partial")
    if output.exists() or partial.exists():
        raise ValueError("output or unfinished tensor release exists; use a new output path")
    path = Path(release) if release is not None else Path(manifest_path).parent / "release.json"
    if path.is_dir():
        path = path / "release.json"
    output.parent.mkdir(parents=True, exist_ok=True)
    progress = _Progress(output)
    progress.update("waiting_for_extraction_release", force=True, release_path=str(path.resolve()))
    try:
        while True:
            if output.exists() or partial.exists():
                raise ValueError("output or unfinished tensor release appeared while waiting")
            failure_path = path.parent / "failure.json"
            if failure_path.exists():
                failure = json.loads(failure_path.read_text(encoding="utf-8"))
                raise ValueError(f"Extraction release failed: {failure.get('error', 'unknown failure')}")
            if path.exists():
                value = json.loads(path.read_text(encoding="utf-8"))
                if (value.get("format_version") != 1 or value.get("complete") is not True or
                        value.get("stage") != "extraction_release" or
                        value.get("training_started") is not False or value.get("automatic_training") is not False):
                    raise ValueError("existing extraction release is incomplete or invalid; refusing to replace or wait past it")
                progress.update("extraction_release_ready", force=True)
                return path
            progress.update()
            time.sleep(poll_seconds)
    except Exception as error:
        progress.update("failed", force=True, error=str(error))
        raise


class _Writer:
    def __init__(self, directory, rows, shard_rows, feature_count):
        self.directory = directory
        self.total = rows
        self.shard_rows = shard_rows
        self.feature_count = feature_count
        self.position = 0
        self.offset = 0
        self.arrays = None
        self.records = []

    def _open(self):
        count = min(self.shard_rows, self.total - self.position)
        index = len(self.records)
        self.current = {"start": self.position, "rows": count}
        self.arrays = {}
        for name, dtype, shape in (("features", "<f4", (count, self.feature_count)),
                                   ("metadata", METADATA_DTYPE, (count,))):
            file = f"shard-{index:06d}-{name}.npy"
            self.current[name] = {"file": file}
            self.arrays[name] = np.lib.format.open_memmap(
                self.directory / file, mode="w+", dtype=dtype, shape=shape)
        self.offset = 0

    def close(self, complete=False):
        if self.arrays is None:
            return
        for array in self.arrays.values():
            array.flush()
            array._mmap.close()
        self.arrays = None
        if complete:
            for name in ("features", "metadata"):
                self.current[name]["sha256"] = sha256(self.directory / self.current[name]["file"])
            self.records.append(self.current)

    def append(self, rows):
        if self.position + len(rows) > self.total:
            raise ValueError("more sample rows than extraction results declared")
        done = 0
        while done < len(rows):
            if self.arrays is None:
                self._open()
            take = min(len(rows) - done, self.current["rows"] - self.offset)
            source = rows[done:done + take]
            target = slice(self.offset, self.offset + take)
            self.arrays["features"][target] = source["features"]
            for field in METADATA_DTYPE.names:
                self.arrays["metadata"][field][target] = source[field]
            done += take
            self.position += take
            self.offset += take
            if self.offset == self.current["rows"]:
                self.close(complete=True)


def pack(manifest_path, games_root, output, shard_rows=16384, allow_synthetic=False, release=None):
    """Pack selected games bound to a frozen release; never mutate source files.

    Human data requires sibling release.json/sources.json, or an explicit release
    path. Only explicitly permitted synthetic fixtures may omit release binding.
    Mutable progress is written to <output>.status.json every fifteen seconds.
    """
    positive_integer(shard_rows, "shard rows")
    manifest_path, games_root, output = map(Path, (manifest_path, games_root, output))
    partial = output.with_name(output.name + ".partial")
    if output.exists() or partial.exists():
        raise ValueError("output or unfinished tensor release exists; use a new output path")
    output.parent.mkdir(parents=True, exist_ok=True)
    progress = _Progress(output)
    progress.update("verifying_release", force=True)
    initial_code_hashes = _code_hashes()
    try:
        result = _pack(manifest_path, games_root, output, shard_rows, allow_synthetic,
                       release, progress, initial_code_hashes)
        progress.update("complete", force=True, samples_processed=result["rows"],
                        samples_total=result["rows"])
        return result
    except Exception as error:
        progress.update("failed", force=True, error=str(error))
        raise


def _pack(manifest_path, games_root, output, shard_rows, allow_synthetic, release,
          progress, initial_code_hashes):
    partial = output.with_name(output.name + ".partial")
    schema = load_schema()
    raw_manifest = manifest_path.read_bytes()
    manifest = json.loads(raw_manifest)
    game_map = validate_manifest(manifest, schema, allow_synthetic)
    frozen_sources, release_provenance = _bind_release(
        manifest, hashlib.sha256(raw_manifest).hexdigest(), manifest_path, game_map, release)
    selected = sorted((g for g in game_map.values() if g["split"] != "test"),
                      key=lambda g: g["game_id"])
    if not all(any(g["split"] == s for g in selected) for s in ("train", "validation")):
        raise ValueError("train and validation games required")
    sources = []
    progress.update("scanning_sources", force=True, games_total=len(selected))
    for game in selected:
        directory = games_root / game["replay_sha256"]
        result_path = directory / "result.json"
        raw_result = result_path.read_bytes()
        result = json.loads(raw_result)
        result_digest = hashlib.sha256(raw_result).hexdigest()
        if result.get("status") != "extracted" or result.get("game_id") != game["game_id"]:
            raise ValueError("game extraction result is missing or mismatched")
        count = positive_integer(result.get("samples"), "extracted sample count")
        digest = result.get("samples_sha256")
        if (not isinstance(digest, str) or len(digest) != 64 or
                any(c not in "0123456789abcdef" for c in digest)):
            raise ValueError("invalid source sample checksum")
        if frozen_sources is not None:
            frozen = frozen_sources[game["game_id"]]
            if (Path(frozen["directory"]).resolve() != directory.resolve() or
                    frozen.get("result_sha256") != result_digest or
                    frozen.get("samples_sha256") != digest or
                    frozen.get("actions_sha256") != result.get("actions_sha256")):
                raise ValueError(f"game source differs from frozen release: {game['game_id']}")
        sources.append({"game_id": game["game_id"], "samples": count,
                        "result_path": str(result_path.resolve()),
                        "result_sha256": result_digest,
                        "samples_path": str((directory / "samples.jsonl.gz").resolve()),
                        "samples_sha256": digest})
        progress.update(games_scanned=len(sources))
    total = sum(source["samples"] for source in sources)
    progress.update("packing", force=True, samples_total=total, games_scanned=len(sources))
    partial.mkdir()
    writer = _Writer(partial, total, shard_rows, len(schema["features"]))
    row_dtype = np.dtype([("features", "<f4", (len(schema["features"]),)), *METADATA_DTYPE.descr])
    row = np.zeros(1, dtype=row_dtype)
    sequences, game_records, split_counts = [], [], Counter()
    samples_processed = 0
    try:
        for game_index, (game, source) in enumerate(zip(selected, sources)):
            sample_path = Path(source["samples_path"])
            if sha256(sample_path) != source["samples_sha256"]:
                raise ValueError(f"source sample checksum mismatch: {game['game_id']}")
            spool_paths = [partial / f"perspective-{p}.spool" for p in (0, 1)]
            spools = [path.open("wb") for path in spool_paths]
            previous, first_frames, counts = {}, {}, Counter()
            try:
                with gzip.open(sample_path, "rt", encoding="utf-8") as stream:
                    for line_number, line in enumerate(stream, 1):
                        try:
                            value = json.loads(line)
                            validated = validate_sample(value, {game["game_id"]: game}, schema)
                            gid, perspective, frame, action_frame, blob, mask, label, weight = validated
                            if perspective in previous and frame <= previous[perspective]:
                                raise ValueError("non-increasing perspective frames")
                            if action_frame - frame >= 24:
                                raise ValueError("label outside original 24-frame window")
                            row["features"][0] = np.frombuffer(blob, dtype="<f4")
                            row["masks"][0], row["labels"][0], row["weights"][0] = mask, label, weight
                            row["frames"][0], row["action_frames"][0] = frame, action_frame
                            row["perspectives"][0], row["game_indices"][0] = perspective, game_index
                            row["delta_frames"][0] = frame - previous[perspective] if perspective in previous else 0
                            spools[perspective].write(row.tobytes())
                            previous[perspective] = frame
                            first_frames.setdefault(perspective, frame)
                            counts[perspective] += 1
                            samples_processed += 1
                            if samples_processed % 1024 == 0:
                                progress.update(samples_processed=samples_processed)
                        except (ValueError, TypeError, KeyError) as error:
                            raise ValueError(f"{game['game_id']} sample line {line_number}: {error}") from error
            finally:
                for spool in spools:
                    spool.close()
            if sum(counts.values()) != source["samples"]:
                raise ValueError("source sample count differs from extraction result")
            if sha256(sample_path) != source["samples_sha256"]:
                raise ValueError("source samples changed while packing")
            if sha256(source["result_path"]) != source["result_sha256"]:
                raise ValueError("source extraction result changed while packing")
            other = next((race for race in game["races"] if race != "P"), "P")
            matchup = "Pv" + other
            game_records.append({**game, "matchup": matchup, "sample_count": sum(counts.values())})
            for perspective, path in enumerate(spool_paths):
                count = counts[perspective]
                if count:
                    sequences.append({"game_index": game_index, "game_id": game["game_id"],
                                      "perspective": perspective, "split": game["split"],
                                      "matchup": matchup, "start": writer.position, "count": count,
                                      "first_frame": first_frames[perspective], "last_frame": previous[perspective]})
                    with path.open("rb") as spool:
                        while True:
                            block = np.fromfile(spool, dtype=row_dtype, count=min(shard_rows, 4096))
                            if not len(block):
                                break
                            writer.append(block)
                    split_counts[game["split"]] += count
                path.unlink()
            progress.update(samples_processed=samples_processed, games_packed=game_index + 1)
        if writer.position != total or writer.arrays is not None:
            raise ValueError("incomplete tensor output")
        progress.update("verifying_output", force=True, samples_processed=samples_processed)
        for source in sources:
            if sha256(source["result_path"]) != source["result_sha256"]:
                raise ValueError("source extraction result changed before completion")
            progress.update()
        if sha256(manifest_path) != hashlib.sha256(raw_manifest).hexdigest():
            raise ValueError("source manifest changed while packing")
        if release_provenance is not None:
            for name in ("release", "sources"):
                if sha256(release_provenance[name + "_path"]) != release_provenance[name + "_sha256"]:
                    raise ValueError("frozen extraction release changed while packing")
        if _code_hashes() != initial_code_hashes:
            raise ValueError("packing implementation changed during tensor preparation")
        result = {"format": FORMAT, "complete": True, "schema": schema,
                  "source": manifest["source"], "source_manifest": str(manifest_path.resolve()),
                  "source_manifest_sha256": hashlib.sha256(raw_manifest).hexdigest(),
                  "extraction_release": release_provenance,
                  "packed_splits": ["train", "validation"], "excluded_test_games": sorted(
                      g["game_id"] for g in game_map.values() if g["split"] == "test"),
                  "test_samples_opened": False, "rows": total, "counts": dict(split_counts),
                  "shard_rows": shard_rows, "weights": "confidence_times_player_quality_without_inverse_game_length",
                  "sources": sources, "games": game_records, "sequences": sequences,
                  "shards": writer.records,
                  "packer_sources": initial_code_hashes}
        (partial / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
        partial.rename(output)
        return result
    finally:
        writer.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--games-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--release", type=Path, help="frozen release.json; defaults beside --manifest")
    parser.add_argument("--wait-for-release", action="store_true",
                        help="wait for a new extraction release before packing; never start training")
    parser.add_argument("--poll-seconds", type=int, default=15, help="release wait interval, 1-30 seconds")
    parser.add_argument("--shard-rows", type=int, default=16384)
    parser.add_argument("--allow-synthetic", action="store_true", help="explicit test fixtures only")
    args = parser.parse_args()
    if args.wait_for_release:
        wait_for_release(args.manifest, args.output, args.release, args.poll_seconds)
    result = pack(args.manifest, args.games_root, args.output, args.shard_rows, args.allow_synthetic, args.release)
    print(json.dumps({"output": str(args.output), "rows": result["rows"], "counts": result["counts"]}))


if __name__ == "__main__":
    main()
