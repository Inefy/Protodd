"""Fail-closed pre-fit checks for full whole-game teacher fits."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re


SCHEMA = "protodd-whole-game-pre-fit-gate-v1"
AUDIT_SCHEMA = "protodd-whole-game-representation-audit-v1"
PARITY_SCHEMA = "protodd-whole-game-multislot-cpu-parity-v1"
MATCHUPS = ("PvP", "PvT", "PvZ")
AUDIT_SOURCE_FILES = (
    "whole_game_representation_audit.py",
    "whole_game_cadence_sequences.py",
    "whole_game_shards.py",
)
PREDECLARED_LIMITS = {
    "cohort_selection": "lexicographically first game IDs within each split/matchup",
    "games_per_matchup": 2,
    "maximum_frame": 3600,
    "maximum_slots": 6,
    "window_frames": 24,
}


def _read_json(path: Path, description: str) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise ValueError(f"{description} is unreadable: {path}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{description} must be a JSON object: {path}")
    return value


def _sha256(path: Path) -> str:
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError as error:
        raise ValueError(f"cannot hash required pre-fit input: {path}") from error


def _validate_release(path: Path, split: str, audit: dict):
    release = _read_json(path / "release.json", f"{split} release receipt")
    if release.get("complete") is not True:
        raise ValueError(f"{split} release is incomplete")
    if release.get("training_ready") is not True:
        raise ValueError(f"{split} release is not training_ready")
    identity_path = path / "identity.json"
    identity = _read_json(identity_path, f"{split} release identity")
    identity_sha = _sha256(identity_path)
    report_release = audit.get("releases", {}).get(split, {})
    if report_release.get("identity_sha256") != identity_sha:
        raise ValueError(f"{split} representation audit identity differs from the release")
    if report_release.get("release_training_ready") is not True:
        raise ValueError(f"{split} representation audit does not certify a training-ready release")
    selected = identity.get("selected")
    if not isinstance(selected, list):
        raise ValueError(f"{split} release identity has no selected cohort")
    if any(not isinstance(row, dict) for row in selected):
        raise ValueError(f"{split} release identity contains a malformed selected row")
    rows = [row for row in selected if row.get("split") == split]
    if not rows:
        raise ValueError(f"{split} release identity has no selected {split} rows")
    if any(not isinstance(row.get("game_id"), str) or not row["game_id"] or
           row.get("matchup") not in MATCHUPS or
           not isinstance(row.get("replay_sha256"), str) or
           re.fullmatch(r"[0-9a-f]{64}", row["replay_sha256"]) is None
           for row in rows):
        raise ValueError(f"{split} release identity contains an invalid game or replay hash")
    if len(rows) != len({row["game_id"] for row in rows}):
        raise ValueError(f"{split} release identity contains duplicate game IDs")

    split_report = audit.get(split, {})
    cohorts = split_report.get("cohort_game_ids", {})
    if not isinstance(cohorts, dict):
        raise ValueError(f"{split} representation cohort is missing")
    for matchup in MATCHUPS:
        selected_ids = sorted({
            row["game_id"] for row in rows if row.get("matchup") == matchup
        })
        expected = selected_ids[:PREDECLARED_LIMITS["games_per_matchup"]]
        if len(expected) != PREDECLARED_LIMITS["games_per_matchup"]:
            raise ValueError(f"{split} release has too few selected games for {matchup}")
        if cohorts.get(matchup) != expected:
            raise ValueError(f"{split}/{matchup} audit cohort differs from the predeclared release selection")
    return identity_sha, {row["game_id"] for row in rows}, {
        row["replay_sha256"] for row in rows
    }


def _validate_audit_metrics(audit: dict) -> set[str]:
    if audit.get("schema") != AUDIT_SCHEMA:
        raise ValueError("representation audit schema is unsupported")
    if audit.get("predeclared_limits") != PREDECLARED_LIMITS:
        raise ValueError("representation audit does not use the predeclared cohort and limits")
    expected_sources = {
        name: _sha256(Path(__file__).with_name(name)) for name in AUDIT_SOURCE_FILES
    }
    source_hashes = audit.get("source_sha256")
    if not isinstance(source_hashes, dict) or any(
            source_hashes.get(name) != digest for name, digest in expected_sources.items()):
        raise ValueError("representation audit source code is stale or unbound")

    safety = audit.get("safety", {})
    for field in ("final_test_read", "optimizer_run", "train_validation_game_overlap",
                  "train_validation_replay_overlap", "validation_labels_modified"):
        if safety.get(field) is not False:
            raise ValueError(f"representation audit safety check failed: {field}")

    train_ids: set[str] = set()
    validation_ids: set[str] = set()
    for split, identifiers in (("train", train_ids), ("validation", validation_ids)):
        split_report = audit.get(split, {})
        cohorts = split_report.get("cohort_game_ids", {})
        if not isinstance(cohorts, dict):
            raise ValueError(f"{split} representation cohort is missing")
        for matchup in MATCHUPS:
            game_ids = cohorts.get(matchup)
            if (not isinstance(game_ids, list) or len(game_ids) != 2 or
                    any(not isinstance(game_id, str) or not game_id for game_id in game_ids)):
                raise ValueError(f"{split}/{matchup} does not contain the predeclared two-game cohort")
            identifiers.update(game_ids)
        if len(identifiers) != 6:
            raise ValueError(f"{split} representation cohort contains duplicate game IDs")

        overall = split_report.get("overall", {})
        complete = overall.get("complete_windows_within_frame_limit")
        events = overall.get("event_conditioned_windows", {})
        action = events.get("action")
        no_action = events.get("no_action")
        action_rate = events.get("action_rate_among_complete")
        if (not isinstance(complete, int) or complete <= 0 or
                not isinstance(action, int) or not isinstance(no_action, int) or
                action < 0 or no_action < 0 or action + no_action != complete or
                not isinstance(action_rate, (int, float)) or not math.isfinite(action_rate) or
                not 0 <= action_rate <= 1 or abs(action_rate - action / complete) > 1e-9):
            raise ValueError(f"{split} event-conditioned window metrics are incomplete or inconsistent")
        labels = overall.get("all_complete_windows", {})
        required_metrics = (
            "stop_slots_by_ordinal", "overflow_commands_beyond_slot_limit",
            "actor_set_availability", "actor_ids_absent_from_causal_observation",
            "entity_target_availability", "position_labels", "delay_frames",
        )
        if any(field not in labels for field in required_metrics):
            raise ValueError(f"{split} representation metrics omit a required target audit")
        if "censored_final_windows_full_game" not in overall:
            raise ValueError(f"{split} representation metrics omit censored final windows")

    if train_ids & validation_ids:
        raise ValueError("representation audit game cohorts overlap")
    return train_ids | validation_ids


def _validate_native_parity(parity: dict) -> None:
    if parity.get("schema") != PARITY_SCHEMA or parity.get("passed") is not True:
        raise ValueError("native export parity did not pass")
    tolerance = parity.get("tolerance")
    maximum_error = parity.get("max_abs_error")
    if (not isinstance(tolerance, (int, float)) or not math.isfinite(tolerance) or tolerance <= 0 or
            not isinstance(maximum_error, (int, float)) or not math.isfinite(maximum_error) or
            maximum_error < 0 or maximum_error > tolerance):
        raise ValueError("native export parity error exceeds its recorded tolerance")
    cases = parity.get("cases")
    if not isinstance(cases, list):
        raise ValueError("native export parity cases are missing")
    case_matchups = set()
    for case in cases:
        if not isinstance(case, dict):
            raise ValueError("native export parity contains a malformed case")
        matchup = case.get("matchup")
        case_error = case.get("max_abs_error")
        if (matchup not in MATCHUPS or not isinstance(case_error, (int, float)) or
                not math.isfinite(case_error) or case_error < 0 or case_error > tolerance):
            raise ValueError("native export parity contains a failed case")
        case_matchups.add(matchup)
    if case_matchups != set(MATCHUPS):
        raise ValueError("native export parity must cover PvP, PvT, and PvZ")
    for field in ("checkpoint_sha256", "package_sha256", "executable_sha256"):
        digest = parity.get(field)
        if not isinstance(digest, str) or len(digest) != 64:
            raise ValueError(f"native export parity is missing {field}")


def verify_pre_fit_gate(train_release, validation_release,
                        representation_audit, native_parity_report) -> dict:
    """Verify all frozen preconditions before constructing a fit or optimizer."""
    train_path = Path(train_release).resolve()
    validation_path = Path(validation_release).resolve()
    audit_path = Path(representation_audit).resolve()
    parity_path = Path(native_parity_report).resolve()
    audit = _read_json(audit_path, "representation audit")
    parity = _read_json(parity_path, "native export parity report")
    train_identity, train_game_ids, train_replay_hashes = _validate_release(
        train_path, "train", audit)
    validation_identity, validation_game_ids, validation_replay_hashes = _validate_release(
        validation_path, "validation", audit)
    if train_game_ids & validation_game_ids:
        raise ValueError("training and validation release game IDs overlap")
    if train_replay_hashes & validation_replay_hashes:
        raise ValueError("training and validation release replay assets overlap")
    _validate_audit_metrics(audit)
    _validate_native_parity(parity)
    return {
        "schema": SCHEMA,
        "passed": True,
        "train_identity_sha256": train_identity,
        "validation_identity_sha256": validation_identity,
        "representation_audit_sha256": _sha256(audit_path),
        "native_parity_report_sha256": _sha256(parity_path),
        "native_parity_max_abs_error": parity["max_abs_error"],
        "native_parity_tolerance": parity["tolerance"],
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("train_release", type=Path)
    parser.add_argument("validation_release", type=Path)
    parser.add_argument("representation_audit", type=Path)
    parser.add_argument("native_parity_report", type=Path)
    args = parser.parse_args()
    print(json.dumps(verify_pre_fit_gate(
        args.train_release, args.validation_release,
        args.representation_audit, args.native_parity_report), indent=2))


if __name__ == "__main__":
    main()
