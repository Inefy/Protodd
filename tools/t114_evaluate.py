#!/usr/bin/env python3
"""Validate and analyze the frozen T114 matched development schedule."""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import re
import statistics
import sys
import zipfile
from pathlib import Path
from typing import Any

TOOLS_DIR = str(Path(__file__).resolve().parent)
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)
from direct_report import parse_match_identity


def _sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _canonical_json(value: Any) -> bytes:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False).encode("utf-8")


def _powershell_compact_json(value: Any) -> bytes:
    # Match ConvertTo-Json -Compress, which preserves ordered configuration keys.
    return json.dumps(value, separators=(",", ":"), ensure_ascii=True).encode("utf-8")


def schedule_digest(schedule: dict[str, Any]) -> str:
    payload = dict(schedule)
    payload.pop("schedule_sha256", None)
    return _sha256_bytes(_canonical_json(payload))


def source_inventory_digest(files: list[dict[str, Any]]) -> str:
    rows = []
    for item in sorted(files, key=lambda entry: entry["path"]):
        rows.append(f"{item['path']}\t{item['sha256'].lower()}\n")
    return _sha256_bytes("".join(rows).encode("utf-8"))


def verify_package(schedule_dir: Path, arm: dict[str, Any], expected: dict[str, Any]) -> list[str]:
    errors: list[str] = []
    package_path = (schedule_dir / arm["package_path"]).resolve()
    try:
        package_path.relative_to(schedule_dir.resolve())
    except ValueError:
        return ["package path escapes the schedule directory"]
    if not package_path.is_file():
        return [f"package missing: {package_path}"]
    if _sha256_file(package_path) != expected["package_sha256"].lower():
        errors.append("package archive hash differs from schedule")
        return errors

    try:
        with zipfile.ZipFile(package_path) as archive:
            manifest = json.loads(archive.read("manifest.json"))
            build = manifest["build_manifest"]
            dll_hash = _sha256_bytes(archive.read("Protodd.dll"))
            if manifest.get("schema") != "protodd-tournament-package-v1":
                errors.append("unsupported tournament package manifest schema")
            if build.get("schema") != "protodd-build-v1":
                errors.append("unsupported build manifest schema")
            if dll_hash != expected["dll_sha256"].lower():
                errors.append("packaged DLL hash differs from schedule")
            if manifest.get("package_dll_sha256", "").lower() != dll_hash:
                errors.append("package manifest DLL hash does not match packaged DLL")
            source_files = build.get("source_files")
            if not isinstance(source_files, list) or not source_files:
                errors.append("package manifest has no source inventory")
            else:
                seen: set[str] = set()
                for entry in source_files:
                    relative = entry.get("path", "")
                    if not relative or relative.startswith(("/", "\\")) or ".." in Path(relative.replace("\\", "/")).parts:
                        errors.append(f"unsafe source inventory path: {relative!r}")
                        continue
                    if relative in seen:
                        errors.append(f"duplicate source inventory path: {relative}")
                        continue
                    seen.add(relative)
                    archive_name = "source/" + relative.replace("\\", "/")
                    try:
                        data = archive.read(archive_name)
                    except KeyError:
                        errors.append(f"source file missing from package: {relative}")
                        continue
                    if _sha256_bytes(data) != entry.get("sha256", "").lower():
                        errors.append(f"source file hash differs from package manifest: {relative}")
                    if len(data) != entry.get("size_bytes"):
                        errors.append(f"source file size differs from package manifest: {relative}")
                computed = source_inventory_digest(source_files)
                if computed != build.get("source_snapshot_sha256", "").lower():
                    errors.append("source inventory digest differs from build manifest")
                if computed != expected["source_snapshot_sha256"].lower():
                    errors.append("source inventory digest differs from schedule")
            if build.get("dll_sha256", "").lower() != dll_hash:
                errors.append("build manifest DLL hash does not match packaged DLL")
    except (KeyError, OSError, ValueError, zipfile.BadZipFile) as error:
        errors.append(f"cannot validate package manifest: {error}")
    return errors


def verify_schedule(schedule: dict[str, Any], schedule_dir: Path, repo_root: Path | None = None) -> list[str]:
    errors: list[str] = []
    if schedule_digest(schedule) != schedule.get("schedule_sha256", "").lower():
        errors.append("schedule hash mismatch")
    if schedule.get("status") != "frozen-development-schedule-not-yet-executed":
        errors.append("unexpected schedule status")
    if schedule.get("development", {}).get("pair_count") != len(schedule.get("pairs", [])):
        errors.append("development pair count does not match the schedule")
    pairs = schedule.get("pairs", [])
    ids = [pair.get("pair_id") for pair in pairs]
    if len(ids) != len(set(ids)):
        errors.append("duplicate pair IDs")
    if len(pairs) != 24:
        errors.append("development schedule must contain 24 pairs")
    expected_strata_count = len({(pair.get("matchup"), pair.get("map")) for pair in pairs})
    if expected_strata_count != 12:
        errors.append("development schedule must contain 12 matchup/map strata")
    runtime_sets: set[str] = set()
    seed_values_by_stratum: dict[tuple[Any, Any], list[Any]] = {}
    for pair in pairs:
        arms = [run.get("arm") for run in pair.get("runs", [])]
        if arms != pair.get("arm_order") or sorted(arms) != ["candidate", "reference"]:
            errors.append(f"invalid arm order or arm coverage in pair {pair.get('pair_id')}")
        expected_order = ["reference", "candidate"] if pair.get("ordinal", 0) % 2 else ["candidate", "reference"]
        if arms != expected_order:
            errors.append(f"arm order does not alternate in pair {pair.get('pair_id')}")
        seed_values_by_stratum.setdefault((pair.get("matchup"), pair.get("map")), []).append(pair.get("seed"))
        if pair.get("map") not in schedule.get("maps", {}):
            errors.append(f"unknown map in pair {pair.get('pair_id')}")
        if pair.get("opponent") not in schedule.get("opponents", {}):
            errors.append(f"unknown opponent in pair {pair.get('pair_id')}")
        for run in pair.get("runs", []):
            runtime_set = run.get("runtime_set")
            if runtime_set in runtime_sets:
                errors.append(f"runtime set reused: {runtime_set}")
            runtime_sets.add(runtime_set)
            if run.get("label") != pair.get("pair_id"):
                errors.append(f"run label differs from pair ID in {pair.get('pair_id')}")
    if len(runtime_sets) != 48:
        errors.append("development arms must use 48 unique runtime sets")
    expected_seeds = sorted(schedule.get("development", {}).get("seeds", []))
    if any(sorted(values) != expected_seeds for values in seed_values_by_stratum.values()):
        errors.append("each matchup/map stratum must have exactly the frozen seed set")
    for key in ("reference", "candidate"):
        arm = schedule.get("arms", {}).get(key)
        if not arm:
            errors.append(f"missing {key} arm")
            continue
        errors.extend(f"{key}: {error}" for error in verify_package(schedule_dir, arm, arm))
    if schedule.get("arms", {}).get("reference", {}).get("package_path") == schedule.get("arms", {}).get("candidate", {}).get("package_path"):
        errors.append("reference and candidate use the same package path")
    ref_zip = schedule_dir / schedule["arms"]["reference"]["package_path"]
    cand_zip = schedule_dir / schedule["arms"]["candidate"]["package_path"]
    try:
        with zipfile.ZipFile(ref_zip) as ref_archive, zipfile.ZipFile(cand_zip) as cand_archive:
            ref_manifest = json.loads(ref_archive.read("manifest.json"))
            cand_manifest = json.loads(cand_archive.read("manifest.json"))
            if ref_manifest["build_manifest"].get("cmake_options") != cand_manifest["build_manifest"].get("cmake_options"):
                errors.append("comparison arm CMake options differ")
            if ref_manifest["build_manifest"].get("instrumentation_patch") != cand_manifest["build_manifest"].get("instrumentation_patch"):
                errors.append("comparison arm instrumentation differs")
    except (KeyError, OSError, ValueError, zipfile.BadZipFile) as error:
        errors.append(f"cannot compare arm manifests: {error}")

    for key, map_info in schedule.get("maps", {}).items():
        path = (schedule_dir / map_info.get("evidence_path", "")).resolve()
        try:
            path.relative_to(schedule_dir.resolve())
        except ValueError:
            errors.append(f"map path escapes schedule directory: {key}")
            continue
        if not path.is_file():
            errors.append(f"map evidence missing: {key}")
        elif _sha256_file(path) != map_info.get("sha256", "").lower():
            errors.append(f"map evidence hash mismatch: {key}")
    if repo_root is not None:
        for opponent_key, opponent in schedule.get("opponents", {}).items():
            name = opponent.get("name", "")
            if not re.fullmatch(r"[A-Za-z0-9_-]+", name):
                errors.append(f"unsafe opponent name: {opponent_key}")
                continue
            package_root = repo_root / "ladder" / "bots" / name
            metadata_path = package_root / ".astra-ladder.json"
            dll_path = package_root / "AI" / f"{name}.dll"
            if not metadata_path.is_file():
                errors.append(f"opponent sidecar missing: {name}")
            else:
                try:
                    metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
                    if metadata.get("sha256", "").lower() != opponent.get("declared_package_sha256", "").lower():
                        errors.append(f"opponent declared package hash differs: {name}")
                    if opponent.get("version") and metadata.get("version") != opponent["version"]:
                        errors.append(f"opponent package version differs: {name}")
                except (OSError, json.JSONDecodeError) as error:
                    errors.append(f"opponent sidecar unreadable: {name}: {error}")
            if not dll_path.is_file():
                errors.append(f"opponent DLL missing: {name}")
            elif _sha256_file(dll_path) != opponent.get("dll_sha256", "").lower():
                errors.append(f"opponent DLL hash differs: {name}")
            archive_hash = opponent.get("archive_sha256")
            if archive_hash:
                archive_path = repo_root / "build" / "tournament-bot-downloads" / f"{name}.zip"
                if not archive_path.is_file():
                    errors.append(f"opponent archive missing: {name}")
                elif _sha256_file(archive_path) != archive_hash.lower():
                    errors.append(f"opponent archive hash differs: {name}")
    return errors


def score_result(result: str | None) -> float | None:
    if not isinstance(result, str):
        return None
    match = re.fullmatch(r"END,(win|draw|loss),\d+", result.strip(), flags=re.IGNORECASE)
    if not match:
        return None
    return {"win": 1.0, "draw": 0.5, "loss": 0.0}[match.group(1).lower()]


def _attempt_paths(archive_root: Path, run: dict[str, Any]) -> tuple[Path, Path, Path]:
    label = run.get("label", "")
    runtime_set = run.get("runtime_set", "")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", label) or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]*", runtime_set):
        raise ValueError("unsafe run label or runtime set in schedule")
    directory = (archive_root / runtime_set).resolve()
    try:
        directory.relative_to(archive_root.resolve())
    except ValueError as error:
        raise ValueError("run path escapes archive root") from error
    return directory / f"{label}.json", directory / f"{label}.preflight.json", directory / f"{label}.raw.log"


def _config_value(config: dict[str, Any], *path: str) -> Any:
    value: Any = config
    for item in path:
        if not isinstance(value, dict):
            return None
        value = value.get(item)
    return value


def _same(value: Any, expected: Any) -> bool:
    if isinstance(value, str) and isinstance(expected, str):
        return value.lower() == expected.lower()
    return value == expected


def validate_attempt(
    schedule: dict[str, Any], pair: dict[str, Any], run: dict[str, Any], arm_name: str,
    archive_root: Path,
) -> dict[str, Any]:
    record_path, preflight_path, raw_path = _attempt_paths(archive_root, run)
    outcome: dict[str, Any] = {
        "arm": arm_name,
        "label": run["label"],
        "runtime_set": run["runtime_set"],
        "record_path": str(record_path),
        "status": "missing",
        "errors": [],
        "result": None,
        "score": None,
        "identity": None,
        "paired_inputs": None,
        "trace_sha256": None,
        "replay_sha256": None,
        "failure_attribution": "none",
    }
    if not record_path.is_file():
        outcome["errors"].append("match record missing")
        return outcome
    try:
        record = json.loads(record_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        outcome["status"] = "invalid"
        outcome["errors"].append(f"match record unreadable: {error}")
        return outcome

    expected_arm = schedule["arms"][arm_name]
    map_info = schedule["maps"][pair["map"]]
    opponent = schedule["opponents"][pair["opponent"]]
    shared = schedule["shared_runtime"]
    checks = {
        "label": (record.get("label"), run["label"]),
        "bot DLL hash": (record.get("bot_sha256"), expected_arm["dll_sha256"]),
        "map hash": (record.get("map_sha256"), map_info["sha256"]),
        "requested seed": (record.get("seed_requested"), pair["seed"]),
        "observed seed": (record.get("seed_observed"), pair["seed"]),
        "host race": (record.get("host_race"), shared["host_race"]),
        "opponent name": (record.get("opponent"), opponent["name"]),
        "opponent race": (record.get("opponent_race"), opponent["race"]),
        "opponent DLL hash": (record.get("opponent_sha256"), opponent["dll_sha256"]),
        "host engine hash": (record.get("host_bwapi_sha256"), shared["bwapi_dll_sha256"]),
        "opponent engine hash": (record.get("opponent_bwapi_sha256"), shared["bwapi_dll_sha256"]),
        "runtime profile": (record.get("runtime_profile"), shared["runtime_profile"]),
        "runtime archive hash": (record.get("runtime_profile_archive_sha256"), shared["runtime_archive_sha256"]),
        "frame limit": (record.get("frame_limit"), shared["frame_limit"]),
        "replay saved": (record.get("replay_saved"), shared["save_replay"]),
        "learning preserved": (record.get("learning_preserved"), shared["preserve_learning"]),
        "opponent learning reset": (record.get("opponent_runtime_learning_reset"), True),
    }
    for name, (actual, expected) in checks.items():
        if not _same(actual, expected):
            outcome["errors"].append(f"{name} mismatch: expected {expected!r}, got {actual!r}")
    if record.get("frame_limit_reached"):
        outcome["errors"].append("game reached the frame limit")
    if record.get("runtime_crashes"):
        outcome["errors"].append("runtime crash recorded")
    crash_sides = {str(item.get("side", "unknown")).lower() for item in record.get("runtime_crashes", [])}
    if "protodd" in crash_sides:
        outcome["failure_attribution"] = "candidate-attributable"
    elif crash_sides and crash_sides <= {"opponent"}:
        outcome["failure_attribution"] = "opponent-attributable"
    elif record.get("status") != "completed" or record.get("termination_reason") != "completed":
        outcome["failure_attribution"] = "review-required"
    if record.get("status") != "completed" or record.get("termination_reason") != "completed":
        outcome["errors"].append("attempt did not complete normally")
    if record.get("host_learning_initial") != [] or record.get("opponent_learning_initial") != []:
        outcome["errors"].append("learning state was not empty at match start")
    replay_value = record.get("replay_path")
    replay_hash = record.get("replay_sha256")
    if not replay_value or not replay_hash:
        outcome["errors"].append("completed attempt has no replay path/hash")
    else:
        replay_path = Path(replay_value)
        if not replay_path.is_absolute():
            replay_path = record_path.parent / replay_path
        replay_path = replay_path.resolve()
        try:
            replay_path.relative_to(record_path.parent.resolve())
        except ValueError:
            outcome["errors"].append("replay path escapes its isolated archive set")
        else:
            if not replay_path.is_file():
                outcome["errors"].append("replay file missing")
            else:
                outcome["replay_sha256"] = _sha256_file(replay_path)
                if outcome["replay_sha256"] != str(replay_hash).lower():
                    outcome["errors"].append("replay file hash differs from match record")
                if record.get("replay_bytes") is not None and replay_path.stat().st_size != record["replay_bytes"]:
                    outcome["errors"].append("replay file size differs from match record")
    if not record.get("process_cleanup", {}).get("cleanup_complete", False):
        outcome["errors"].append("process cleanup is not verified complete")

    if not preflight_path.is_file():
        outcome["errors"].append("preflight manifest missing")
        config: dict[str, Any] = {}
    else:
        try:
            preflight_bytes = preflight_path.read_bytes()
            if _sha256_bytes(preflight_bytes) != str(record.get("preflight_manifest_sha256", "")).lower():
                outcome["errors"].append("preflight manifest hash differs from match record")
            preflight = json.loads(preflight_bytes)
            config = preflight.get("configuration", {})
            if preflight.get("configuration_sha256") != _sha256_bytes(_powershell_compact_json(config)):
                outcome["errors"].append("preflight configuration hash is invalid")
        except (OSError, json.JSONDecodeError) as error:
            config = {}
            outcome["errors"].append(f"preflight manifest unreadable: {error}")

    config_checks = {
        "runtime set": (_config_value(config, "runtime_set"), run["runtime_set"]),
        "runtime profile": (_config_value(config, "runtime_profile"), shared["runtime_profile"]),
        "runtime archive hash": (_config_value(config, "runtime_profile_info", "archive_sha256"), shared["runtime_archive_sha256"]),
        "host race": (_config_value(config, "host", "race"), shared["host_race"]),
        "map path": (_config_value(config, "game", "map"), map_info["relative_path"]),
        "map hash": (_config_value(config, "game", "map_sha256"), map_info["sha256"]),
        "frame limit": (_config_value(config, "game", "frame_limit"), shared["frame_limit"]),
        "frame delay": (_config_value(config, "game", "frame_milliseconds"), shared["frame_milliseconds"]),
        "requested seed": (_config_value(config, "game", "seed_requested"), pair["seed"]),
        "replay setting": (_config_value(config, "game", "save_replay") is not None, shared["save_replay"]),
        "observer setting": (_config_value(config, "game", "observer_enabled"), shared["observer_enabled"]),
        "learning preserve setting": (_config_value(config, "learning", "preserve"), shared["preserve_learning"]),
        "opponent name": (_config_value(config, "opponent", "name"), opponent["name"]),
        "opponent race": (_config_value(config, "opponent", "race"), opponent["race"]),
    }
    for name, (actual, expected) in config_checks.items():
        if not _same(actual, expected):
            outcome["errors"].append(f"preflight {name} mismatch: expected {expected!r}, got {actual!r}")
    if not _same(_config_value(config, "game", "timeout_seconds"), None):
        outcome["errors"].append("preflight timeout must be disabled for scheduled runs")

    outcome["paired_inputs"] = {
        "map_sha256": record.get("map_sha256"),
        "seed_requested": record.get("seed_requested"),
        "seed_observed": record.get("seed_observed"),
        "host_race": record.get("host_race"),
        "opponent": record.get("opponent"),
        "opponent_race": record.get("opponent_race"),
        "opponent_sha256": record.get("opponent_sha256"),
        "opponent_components_sha256": record.get("opponent_components_sha256"),
        "opponent_metadata_sha256": record.get("opponent_metadata_sha256"),
        "host_bwapi_sha256": record.get("host_bwapi_sha256"),
        "opponent_bwapi_sha256": record.get("opponent_bwapi_sha256"),
        "runtime_profile": record.get("runtime_profile"),
        "runtime_profile_archive_sha256": record.get("runtime_profile_archive_sha256"),
        "host_runtime_ini_sha256": record.get("host_runtime_ini_sha256"),
        "opponent_runtime_ini_sha256": record.get("opponent_runtime_ini_sha256"),
        "opponent_runtime_configuration_sha256": record.get("opponent_runtime_configuration_sha256"),
        "opponent_runtime_strategy_configuration_sha256": record.get("opponent_runtime_strategy_configuration_sha256"),
        "frame_limit": record.get("frame_limit"),
        "frame_milliseconds": _config_value(config, "game", "frame_milliseconds"),
        "replay_saved": record.get("replay_saved"),
        "learning_preserved": record.get("learning_preserved"),
        "opponent_runtime_learning_reset": record.get("opponent_runtime_learning_reset"),
        "host_learning_initial": record.get("host_learning_initial"),
        "opponent_learning_initial": record.get("opponent_learning_initial"),
    }

    if not raw_path.is_file():
        outcome["errors"].append("raw trace missing")
    else:
        try:
            raw_bytes = raw_path.read_bytes()
            outcome["trace_sha256"] = _sha256_bytes(raw_bytes)
            identity = parse_match_identity(raw_bytes.decode("utf-8", errors="replace").splitlines())
            outcome["identity"] = identity
            if identity is None:
                outcome["errors"].append("raw trace has no MATCH identity header")
            elif identity.get("seed") != pair["seed"]:
                outcome["errors"].append("MATCH header seed differs from scheduled seed")
            elif identity.get("self_start_tile") is None:
                outcome["errors"].append("MATCH header has no realized self start tile")
        except (OSError, ValueError) as error:
            outcome["errors"].append(f"cannot parse raw trace identity: {error}")

    outcome["result"] = record.get("result")
    outcome["score"] = score_result(record.get("result"))
    outcome["status"] = "completed" if record.get("status") == "completed" else "incomplete"
    if outcome["score"] is None:
        outcome["errors"].append("terminal result is not a scored win/draw/loss")
    if outcome["errors"]:
        outcome["status"] = "invalid" if record.get("status") == "completed" else "incomplete"
    return outcome


def _percentile(sorted_values: list[float], probability: float) -> float:
    position = (len(sorted_values) - 1) * probability
    lower = math.floor(position)
    upper = math.ceil(position)
    if lower == upper:
        return sorted_values[lower]
    fraction = position - lower
    return sorted_values[lower] * (1.0 - fraction) + sorted_values[upper] * fraction


def stratified_bootstrap(
    deltas_by_stratum: dict[str, list[float]], seed: int, resamples: int = 10_000,
) -> dict[str, Any]:
    if not deltas_by_stratum or any(not values for values in deltas_by_stratum.values()):
        return {"mean_delta": None, "ci95": None, "resamples": 0}
    strata = sorted(deltas_by_stratum)
    point = statistics.mean(statistics.mean(deltas_by_stratum[stratum]) for stratum in strata)
    rng = random.Random(seed)
    bootstrap_values: list[float] = []
    for _ in range(resamples):
        stratum_means = []
        for stratum in strata:
            values = deltas_by_stratum[stratum]
            sample = [values[rng.randrange(len(values))] for _ in values]
            stratum_means.append(statistics.mean(sample))
        bootstrap_values.append(statistics.mean(stratum_means))
    bootstrap_values.sort()
    return {
        "mean_delta": point,
        "ci95": [_percentile(bootstrap_values, 0.025), _percentile(bootstrap_values, 0.975)],
        "resamples": resamples,
        "prng": "Python random.Random MT19937",
        "seed_u64": seed,
    }


def evaluate_schedule(schedule: dict[str, Any], archive_root: Path) -> dict[str, Any]:
    pairs_out: list[dict[str, Any]] = []
    deltas_by_stratum: dict[str, list[float]] = {}
    candidate_crashes: list[str] = []
    completed_pairs = 0
    for pair in schedule["pairs"]:
        attempts = {}
        for run in pair["runs"]:
            attempts[run["arm"]] = validate_attempt(schedule, pair, run, run["arm"], archive_root)
        reference = attempts.get("reference", {"status": "missing", "errors": ["reference attempt missing"]})
        candidate = attempts.get("candidate", {"status": "missing", "errors": ["candidate attempt missing"]})
        pair_errors: list[str] = []
        ref_id, cand_id = reference.get("identity"), candidate.get("identity")
        if ref_id and cand_id:
            for field in ("seed", "map_hash", "self_start_tile"):
                if ref_id.get(field) != cand_id.get(field):
                    pair_errors.append(f"paired MATCH identity differs for {field}")
        if reference.get("paired_inputs") and candidate.get("paired_inputs"):
            if reference["paired_inputs"] != candidate["paired_inputs"]:
                pair_errors.append("candidate/reference runtime, opponent, map, seed, or learning inputs differ")
        if candidate.get("failure_attribution") in ("candidate-attributable", "review-required"):
            candidate_crashes.append(
                f"{pair['pair_id']}: {candidate['failure_attribution']} incomplete/failed candidate attempt"
            )
        comparable = (
            reference.get("status") == "completed"
            and candidate.get("status") == "completed"
            and not pair_errors
        )
        delta = None
        if comparable:
            delta = candidate["score"] - reference["score"]
            stratum = f"{pair['matchup']}|{pair['map']}"
            deltas_by_stratum.setdefault(stratum, []).append(delta)
            completed_pairs += 1
        pairs_out.append({
            "pair_id": pair["pair_id"],
            "matchup": pair["matchup"],
            "map": pair["map"],
            "seed": pair["seed"],
            "reference": reference,
            "candidate": candidate,
            "pair_errors": pair_errors,
            "candidate_minus_reference_score": delta,
            "strength_pair_valid": comparable,
        })

    expected_strata = sorted({f"{pair['matchup']}|{pair['map']}" for pair in schedule["pairs"]})
    missing_strata = [stratum for stratum in expected_strata if not deltas_by_stratum.get(stratum)]
    seed = int(schedule["schedule_sha256"][:16], 16)
    bootstrap_input = {stratum: deltas_by_stratum.get(stratum, []) for stratum in expected_strata}
    bootstrap = stratified_bootstrap(bootstrap_input, seed)
    all_attempts_complete = all(
        pair["reference"]["status"] == "completed" and pair["candidate"]["status"] == "completed"
        for pair in pairs_out
    )
    return {
        "schema": "protodd-t114-development-evaluation-v1",
        "schedule_id": schedule["schedule_id"],
        "schedule_sha256": schedule["schedule_sha256"],
        "analysis_status": "complete" if all_attempts_complete and completed_pairs == len(schedule["pairs"]) else "incomplete",
        "scheduled_pairs": len(schedule["pairs"]),
        "completed_valid_pairs": completed_pairs,
        "missing_or_invalid_strata": missing_strata,
        "candidate_failure_attention": candidate_crashes,
        "strata": {key: {"n": len(value), "mean_delta": statistics.mean(value)} for key, value in sorted(deltas_by_stratum.items())},
        "paired_bootstrap": bootstrap,
        "method": "Win=1, draw=0.5, loss=0; candidate minus reference; 10,000 pair-level bootstrap resamples within matchup x map strata, then equal-weight mean across strata.",
        "claim_limit": "24 pairs screen large regressions; this analysis alone does not establish a small strength gain or operational qualification.",
        "pairs": pairs_out,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--schedule", type=Path, required=True)
    parser.add_argument("--archive-root", type=Path, default=Path("build/direct-logs/stock-certification"))
    parser.add_argument("--repo-root", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path)
    parser.add_argument("--validate-only", action="store_true", help="verify frozen schedule packages/maps without reading match results")
    args = parser.parse_args()
    try:
        schedule_path = args.schedule.resolve()
        schedule = json.loads(schedule_path.read_text(encoding="utf-8"))
        errors = verify_schedule(schedule, schedule_path.parent, args.repo_root.resolve())
        if errors:
            print(json.dumps({"valid": False, "errors": errors}, indent=2), file=sys.stderr)
            return 2
        if args.validate_only:
            print(json.dumps({"valid": True, "schedule_id": schedule["schedule_id"], "pairs": len(schedule["pairs"]), "schedule_sha256": schedule["schedule_sha256"]}, indent=2))
            return 0
        report = evaluate_schedule(schedule, args.archive_root.resolve())
        rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
        if args.output:
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(rendered, encoding="utf-8")
        print(rendered, end="")
        return 0
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as error:
        print(f"T114 evaluation failed: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
