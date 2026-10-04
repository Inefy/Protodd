#!/usr/bin/env python3
"""Report forecast/outcome alignment diagnostics without claiming calibration."""

from __future__ import annotations

import argparse
from pathlib import Path


def fields(line: str) -> dict[str, str]:
    parts = line.rstrip("\r\n").split(",")
    start = 1 if len(parts) > 1 and "=" in parts[1] else 2
    return {part.split("=", 1)[0]: part.split("=", 1)[1]
            for part in parts[start:] if "=" in part}


def roster(value: str) -> list[tuple[str, int, int, int, bool]]:
    result = []
    if not value:
        return result
    for item in value.split(";"):
        side, unit_id, kind, durability, known = item.split(":")
        result.append((side, int(unit_id), int(kind), int(durability), known == "1"))
    return result


def validate_pair(data: dict[str, str]) -> str:
    """Return an unverified observational alignment or a rejection reason."""
    if data.get("simulation") != "1" or data.get("forecastFrame", "-1") == "-1":
        return "no_forecast"
    if data.get("initialComplete") != "1" or data.get("forecastRosterComplete") != "1":
        return "incomplete_known_units"
    if data.get("outcomeObserved") != "1" or data.get("censor") != "horizon_reached":
        return "censored:" + data.get("censor", "missing")
    try:
        forecast_frame = int(data["forecastFrame"])
        outcome_frame = int(data["outcomeFrame"])
        expected_horizon = int(data["expectedHorizonFrames"])
    except (KeyError, ValueError):
        return "invalid_frames"
    if expected_horizon <= 0 or outcome_frame - forecast_frame != expected_horizon:
        return "frame_mismatch"
    try:
        forecast = roster(data.get("forecastRoster", ""))
        outcome = roster(data.get("outcomeRoster", ""))
    except (ValueError, TypeError):
        return "malformed_roster"
    if (not forecast or len(forecast) != len(outcome) or
            any(not unit[4] for unit in forecast + outcome)):
        return "incomplete_known_units"
    forecast_keys = [(side, unit_id, kind) for side, unit_id, kind, _, _ in forecast]
    outcome_keys = [(side, unit_id, kind) for side, unit_id, kind, _, _ in outcome]
    if len(set(forecast_keys)) != len(forecast_keys) or len(set(outcome_keys)) != len(outcome_keys):
        return "duplicate_unit_id"
    if set(forecast_keys) != set(outcome_keys):
        return "unit_roster_mismatch"
    if data.get("calibrationStatus") != "uncomparable_simulation_power_vs_hp_shields":
        return "unknown_calibration_status"
    coverage = data.get("simulationRosterCoverage", "unknown")
    if coverage != "unknown":
        return "unsupported_simulation_roster_coverage_claim"
    return "observationally_aligned_unverified_simulation"


def audit_lines(lines: list[str]) -> dict[str, object]:
    unique: dict[tuple[str, str, str, str, str], dict[str, str]] = {}
    summaries: dict[str, dict[str, str]] = {}
    duplicate_count = 0
    for line in lines:
        if line.startswith("COMBAT_FORECAST,"):
            parts = line.rstrip("\r\n").split(",", 2)
            data = fields(line)
            key = (data.get("gameId", ""), data.get("opponentId", ""),
                   data.get("seed", ""), data.get("mapHash", ""), parts[1])
            if key in unique:
                duplicate_count += 1
            else:
                unique[key] = data
        elif line.startswith("COMBAT_FORECAST_SUMMARY,"):
            data = fields(line)
            summaries[data.get("gameId", "")] = data

    pair_counts: dict[str, int] = {}
    censor_reason_counts: dict[str, int] = {}
    for data in unique.values():
        status = validate_pair(data)
        pair_counts[status] = pair_counts.get(status, 0) + 1
        reason = data.get("censor", "missing")
        censor_reason_counts[reason] = censor_reason_counts.get(reason, 0) + 1

    records_per_game: dict[str, int] = {}
    for game_id, _opponent_id, _seed, _map_hash, _event_id in unique:
        records_per_game[game_id] = records_per_game.get(game_id, 0) + 1
    summaries_match = all(int(data.get("created", "-1")) == records_per_game.get(game_id, 0)
                          for game_id, data in summaries.items())
    flushes_match = all(int(data.get("flushed", "-1")) == records_per_game.get(game_id, 0)
                        for game_id, data in summaries.items())
    accounted_games = set(records_per_game).issubset(summaries)
    no_audit_data = not unique and not summaries
    complete_eliminations = sum(censor_reason_counts.get(reason, 0)
                                for reason in ("early_resolution_before_horizon",
                                               "friendly_eliminated", "enemy_eliminated"))
    return {
        "unique_records": len(unique),
        "duplicate_records": duplicate_count,
        "observationally_aligned_unverified_simulation_count":
            pair_counts.get("observationally_aligned_unverified_simulation", 0),
        "pair_rejections": {key: value for key, value in pair_counts.items()
                             if key != "observationally_aligned_unverified_simulation"},
        "censor_reason_counts": censor_reason_counts,
        "complete_elimination_records": complete_eliminations,
        "selection_bias_note": (
            "Potentially survivorship-biased subset: encounters that eliminate a side "
            "before the fixed horizon are censored and excluded from aligned diagnostics."),
        "calibration_metrics": None,
        "calibration_metrics_suppressed": True,
        "calibration_status": "uncomparable_simulation_power_vs_hp_shields",
        "simulation_roster_coverage": "unknown",
        "summary_matches": no_audit_data or (bool(summaries) and summaries_match and accounted_games),
        "flush_matches": no_audit_data or (bool(summaries) and flushes_match and accounted_games),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    args = parser.parse_args()
    result = audit_lines(args.log.read_text(encoding="utf-8", errors="replace").splitlines())
    for key, value in result.items():
        print(f"{key}={value}")
    return 0 if result["summary_matches"] and result["flush_matches"] else 2


if __name__ == "__main__":
    raise SystemExit(main())
