#!/usr/bin/env python3
"""Validate paired Protodd PvT portfolio logs and export cumulative v1 history.

The trusted results CSV must come from the independent tournament/evaluation
validator. The bot's PVT_STRATEGY_MATCH_RESULT line is deliberately untrusted.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import re
import tempfile
from collections.abc import Iterable
from pathlib import Path
from typing import Any

VERSION = "r3-siege-v3-pvt-portfolio-v1"
SCHEMA = "pvt-portfolio-validator-state-v3"
PREVIOUS_SCHEMA = "pvt-portfolio-validator-state-v2"
HISTORY_HEADER = "# PROTODD_PVT_PORTFOLIO 1"
ARM_NAMES = {
    "standard",
    "safe-2gateway-range-observer",
    "economic-1gateway-observer",
}
TRUSTED_COLUMNS = (
    "match_id", "validator_id", "opponent", "opponent_race", "map",
    "strategy_version", "strategy", "outcome", "completed", "crashed",
    "externally_validated",
)
MAX_FILE_BYTES = 16 * 1024 * 1024
MAX_LINE_BYTES = 16 * 1024
MAX_LOG_LINES = 250_000
MAX_MATCHES = 50_000
MAX_HISTORY_ROWS = 50_000
MAX_HISTORY_BYTES = 4 * 1024 * 1024
MAX_HISTORY_LINE_BYTES = 2048
MAX_COUNTER = 1_000_000
SAFE_ID = re.compile(r"^[A-Za-z0-9_.:-]{1,128}$")


class ValidationError(ValueError):
    pass


def _bounded_lines(path: Path) -> Iterable[str]:
    total = 0
    with path.open("rb") as source:
        number = 0
        while True:
            # BufferedReader.readline(size) allocates at most the configured
            # cap plus one byte, even for a hostile unterminated line.
            raw = source.readline(MAX_LINE_BYTES + 1)
            if not raw:
                break
            number += 1
            total += len(raw)
            if total > MAX_FILE_BYTES:
                raise ValidationError(f"{path}: file exceeds {MAX_FILE_BYTES} bytes")
            if len(raw) > MAX_LINE_BYTES:
                raise ValidationError(f"{path}:{number}: line exceeds {MAX_LINE_BYTES} bytes")
            try:
                yield raw.decode("utf-8").rstrip("\r\n")
            except UnicodeDecodeError as exc:
                raise ValidationError(f"{path}:{number}: input is not UTF-8") from exc


def _safe_field(value: str, name: str, maximum: int = 256) -> str:
    if not value or len(value) > maximum or any(ord(ch) < 0x20 or ord(ch) == 0x7F for ch in value):
        raise ValidationError(f"invalid {name}")
    if "," in value:
        raise ValidationError(f"invalid comma in {name}")
    return value


def _kv_line(line: str, kind: str) -> dict[str, str] | None:
    prefix = kind + ","
    if not line.startswith(prefix):
        return None
    fields: dict[str, str] = {}
    for piece in line[len(prefix):].split(","):
        key, separator, value = piece.partition("=")
        if not separator or not key or key in fields:
            raise ValidationError(f"malformed {kind} log line")
        fields[key] = value
    return fields


def parse_bot_log(path: Path) -> tuple[dict[str, dict[str, str]], dict[str, dict[str, str]]]:
    selections: dict[str, dict[str, str]] = {}
    results: dict[str, dict[str, str]] = {}
    for number, line in enumerate(_bounded_lines(path), 1):
        if number > MAX_LOG_LINES:
            raise ValidationError(f"{path}: too many log lines")
        for kind, target in (("PVT_STRATEGY_SELECTION", selections),
                             ("PVT_STRATEGY_MATCH_RESULT", results)):
            row = _kv_line(line, kind)
            if row is None:
                continue
            match_id = row.get("match_id", "")
            if not SAFE_ID.fullmatch(match_id):
                continue
            previous = target.get(match_id)
            if previous is not None and previous != row:
                raise ValidationError(f"conflicting {kind} entries for {match_id}")
            target[match_id] = row
            if len(selections) + len(results) > MAX_MATCHES * 2:
                raise ValidationError("too many match log entries")
    return selections, results


def parse_trusted_results(path: Path) -> dict[str, dict[str, str]]:
    rows: dict[str, dict[str, str]] = {}
    iterator = iter(_bounded_lines(path))
    try:
        first = next(iterator)
    except StopIteration:
        raise ValidationError("trusted result ledger is empty")
    header = next(csv.reader([first]))
    if tuple(header) != TRUSTED_COLUMNS:
        raise ValidationError("trusted result ledger has an unexpected header")
    for number, line in enumerate(iterator, 2):
        if not line:
            continue
        values = next(csv.reader([line], strict=True))
        if len(values) != len(TRUSTED_COLUMNS):
            raise ValidationError(f"{path}:{number}: wrong field count")
        row = dict(zip(TRUSTED_COLUMNS, values, strict=True))
        match_id = row["match_id"]
        validator_id = row["validator_id"]
        if not SAFE_ID.fullmatch(match_id) or not SAFE_ID.fullmatch(validator_id):
            raise ValidationError(f"{path}:{number}: invalid match or validator ID")
        for name in ("opponent", "opponent_race", "map", "strategy_version", "strategy"):
            _safe_field(row[name], name)
        # Detect every duplicate/conflicting ID before race, build, crash, or
        # validation eligibility filtering. A rejected row cannot shadow or
        # silently coexist with an already credited result.
        previous = rows.get(match_id)
        if previous is not None and previous != row:
            raise ValidationError(f"conflicting trusted outcomes for {match_id}")
        rows[match_id] = row
        if len(rows) > MAX_MATCHES:
            raise ValidationError("too many trusted match rows")
    return rows


def _match_fingerprint(row: dict[str, str]) -> str:
    identity = [row[name] for name in TRUSTED_COLUMNS]
    return hashlib.sha256(json.dumps(identity, separators=(",", ":")).encode()).hexdigest()


def _empty_state() -> dict[str, Any]:
    return {"schema": SCHEMA, "processed_matches": {}, "records": [],
            "history_sha256": "", "state_sha256": ""}


def _state_fingerprint(state: dict[str, Any]) -> str:
    payload = {key: value for key, value in state.items() if key != "state_sha256"}
    canonical = json.dumps(payload, sort_keys=True, separators=(",", ":"),
                           ensure_ascii=False).encode("utf-8")
    return hashlib.sha256(canonical).hexdigest()


def _seal_state(state: dict[str, Any]) -> None:
    state["state_sha256"] = _state_fingerprint(state)


def _serialize_state(state: dict[str, Any]) -> bytes:
    _seal_state(state)
    return (json.dumps(state, sort_keys=True, separators=(",", ":"),
                        ensure_ascii=False) + "\n").encode("utf-8")


def _read_bounded_bytes(path: Path, maximum_bytes: int) -> bytes:
    if path.stat().st_size > maximum_bytes:
        raise ValidationError(f"{path}: file exceeds {maximum_bytes} bytes")
    with path.open("rb") as source:
        data = source.read(maximum_bytes + 1)
    if len(data) > maximum_bytes:
        raise ValidationError(f"{path}: file exceeds {maximum_bytes} bytes")
    return data


def _load_state(path: Path, history_path: Path) -> dict[str, Any]:
    if path.exists():
        raw = _read_bounded_bytes(path, MAX_FILE_BYTES)
        try:
            state = json.loads(raw)
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValidationError("validator state is corrupt") from exc
        if not isinstance(state, dict) or state.get("schema") not in {SCHEMA, PREVIOUS_SCHEMA}:
            raise ValidationError("validator state schema is unsupported")
        schema = state["schema"]
        expected_keys = {"schema", "processed_matches", "records", "history_sha256"}
        if schema == SCHEMA:
            expected_keys.add("state_sha256")
        if set(state) != expected_keys:
            raise ValidationError("validator state fields are corrupt")
        if schema == SCHEMA:
            state_checksum = state.get("state_sha256")
            if (not isinstance(state_checksum, str) or
                    not re.fullmatch(r"[0-9a-f]{64}", state_checksum) or
                    state_checksum != _state_fingerprint(state)):
                raise ValidationError("validator state checksum is corrupt")
        processed = state.get("processed_matches")
        records = state.get("records")
        if not isinstance(processed, dict) or not isinstance(records, list):
            raise ValidationError("validator state fields are corrupt")
        if schema == PREVIOUS_SCHEMA and processed:
            raise ValidationError(
                "legacy validator state has unchecksummed processed IDs; restore a trusted "
                "state backup or rebuild state from a complete trusted results ledger")
        if len(processed) > MAX_MATCHES or len(records) > MAX_HISTORY_ROWS:
            raise ValidationError("validator state exceeds row limit")
        for match_id, fingerprint in processed.items():
            if (not isinstance(match_id, str) or not SAFE_ID.fullmatch(match_id) or
                    not isinstance(fingerprint, str) or not re.fullmatch(r"[0-9a-f]{64}", fingerprint)):
                raise ValidationError("validator state contains an invalid match identity")
        normalized = _empty_state()
        normalized["processed_matches"] = dict(processed)
        for record in records:
            _validate_state_record(record)
            normalized["records"].append(record)
        history_bytes = _serialize_history(normalized["records"]).encode("utf-8")
        history_digest = hashlib.sha256(history_bytes).hexdigest()
        if schema == SCHEMA and state.get("history_sha256") != history_digest:
            raise ValidationError("validator state history checksum is corrupt")
        if schema == PREVIOUS_SCHEMA and state.get("history_sha256") != history_digest:
            raise ValidationError("legacy validator state history checksum is corrupt")
        normalized["history_sha256"] = history_digest
        _validate_state_records(normalized["records"])
        # Upgrade only an empty-ID v2 state. Historical IDs cannot be checked
        # retroactively, so legacy dedup state is never trusted for replay.
        serialized_state = _serialize_state(normalized)
        if schema == PREVIOUS_SCHEMA:
            _atomic_write(path, serialized_state, MAX_FILE_BYTES)
        if history_path.exists():
            history_matches = False
            if history_path.stat().st_size <= MAX_HISTORY_BYTES:
                try:
                    history_matches = _read_bounded_bytes(
                        history_path, MAX_HISTORY_BYTES) == history_bytes
                except (OSError, ValidationError):
                    history_matches = False
            if not history_matches:
                _atomic_write(history_path, history_bytes, MAX_HISTORY_BYTES)
        else:
            _atomic_write(history_path, history_bytes, MAX_HISTORY_BYTES)
        return normalized
    if history_path.exists():
        records = _read_history(history_path)
        if any(record["wins"] or record["losses"] for record in records):
            raise ValidationError(
                "nonempty history exists without validator dedup state; restore matching state "
                "or start a new output history to avoid recounting old match IDs")
        state = _empty_state()
        state["records"] = records
        return state
    return _empty_state()


def _validate_state_record(record: Any) -> None:
    keys = ("version", "race", "opponent", "map", "strategy", "wins", "losses")
    if not isinstance(record, dict) or set(record) != set(keys):
        raise ValidationError("validator state contains a malformed history row")
    for name in ("version", "race", "opponent", "map", "strategy"):
        value = record[name]
        if not isinstance(value, str):
            raise ValidationError("validator state has a non-string identity field")
        _safe_field(value, name)
    if (record["version"] != VERSION or record["race"] != "terran" or
            record["strategy"] not in ARM_NAMES):
        raise ValidationError("validator state contains an incompatible history row")
    for name in ("wins", "losses"):
        value = record[name]
        if type(value) is not int or value < 0 or value > MAX_COUNTER:
            raise ValidationError("validator state contains an invalid outcome count")


def _read_history(path: Path) -> list[dict[str, Any]]:
    if path.stat().st_size > MAX_HISTORY_BYTES:
        raise ValidationError("existing history exceeds runtime byte limit")
    lines = iter(_bounded_lines(path))
    try:
        if next(lines) != HISTORY_HEADER:
            raise ValidationError("existing history schema is unsupported")
        if next(lines) != "# version,race,opponent,map,strategy,wins,losses":
            raise ValidationError("existing history columns are unsupported")
    except StopIteration as exc:
        raise ValidationError("existing history is truncated") from exc
    records: dict[tuple[str, ...], dict[str, Any]] = {}
    for number, line in enumerate(lines, 3):
        if len(line.encode("utf-8")) + 1 > MAX_HISTORY_LINE_BYTES:
            raise ValidationError(f"existing history row {number} exceeds runtime line limit")
        if not line or line.startswith("#"):
            continue
        fields = line.split(",")
        if len(fields) != 7:
            raise ValidationError(f"existing history row {number} is malformed")
        version, race, opponent, map_name, strategy, wins, losses = fields
        try:
            record = {"version": version, "race": race, "opponent": opponent, "map": map_name,
                      "strategy": strategy, "wins": int(wins), "losses": int(losses)}
        except ValueError as exc:
            raise ValidationError(f"existing history row {number} has invalid counts") from exc
        _validate_state_record(record)
        key = tuple(record[name] for name in ("version", "race", "opponent", "map", "strategy"))
        previous = records.get(key)
        if previous is None:
            records[key] = record
        else:
            previous["wins"] = max(previous["wins"], record["wins"])
            previous["losses"] = max(previous["losses"], record["losses"])
        if len(records) > MAX_HISTORY_ROWS:
            raise ValidationError("existing history exceeds row limit")
    return list(records.values())


def _event_matches(
    selection: dict[str, str], result: dict[str, str], trusted: dict[str, str]
) -> bool:
    if selection.get("source") != "adaptive-portfolio":
        return False
    if result.get("externally_validated") != "0":
        return False
    fields = {"opponent": "opponent", "opponent_race": "opponent_race", "map": "map",
              "strategy_version": "strategy_version"}
    for log_key, trusted_key in fields.items():
        if selection.get(log_key) != trusted.get(trusted_key) or result.get(log_key) != trusted.get(trusted_key):
            return False
    if (selection.get("id") != trusted.get("strategy") or
            result.get("arm") != trusted.get("strategy") or
            result.get("outcome") != trusted.get("outcome")):
        return False
    return (trusted.get("completed") in {"1", "true"} and
            trusted.get("crashed") in {"0", "false"} and
            trusted.get("externally_validated") in {"1", "true"})


def validate_campaign(log_path: Path, trusted_path: Path, state_path: Path,
                      history_path: Path) -> dict[str, int]:
    if state_path.resolve() == history_path.resolve():
        raise ValidationError("state and cumulative history must use separate files")
    selections, raw_results = parse_bot_log(log_path)
    trusted_rows = parse_trusted_results(trusted_path)
    state = _load_state(state_path, history_path)
    records = {tuple(record[name] for name in
                     ("version", "race", "opponent", "map", "strategy")): record
               for record in state["records"]}
    summary = {"added": 0, "duplicate": 0, "rejected": 0}
    processed = state["processed_matches"]
    for match_id, trusted in trusted_rows.items():
        fingerprint = _match_fingerprint(trusted)
        previous = processed.get(match_id)
        if previous is not None:
            if previous != fingerprint:
                raise ValidationError(f"match ID {match_id} was reused with different result data")
            summary["duplicate"] += 1
            continue
        selection = selections.get(match_id)
        result = raw_results.get(match_id)
        if selection is None or result is None or not _event_matches(selection, result, trusted):
            summary["rejected"] += 1
            continue
        key = (trusted["strategy_version"], trusted["opponent_race"], trusted["opponent"],
               trusted["map"], trusted["strategy"])
        record = records.get(key)
        if record is None:
            if len(records) >= MAX_HISTORY_ROWS:
                raise ValidationError("history row limit reached")
            record = {"version": key[0], "race": key[1], "opponent": key[2], "map": key[3],
                      "strategy": key[4], "wins": 0, "losses": 0}
            records[key] = record
        counter = "wins" if trusted["outcome"] == "win" else "losses"
        if record[counter] >= MAX_COUNTER:
            raise ValidationError("history outcome counter limit reached")
        record[counter] += 1
        processed[match_id] = fingerprint
        if len(processed) > MAX_MATCHES:
            raise ValidationError("processed match ID limit reached")
        summary["added"] += 1

    state["records"] = [records[key] for key in sorted(records)]
    state["processed_matches"] = processed
    _validate_state_records(state["records"])
    exported_history = _serialize_history(state["records"]).encode("utf-8")
    state["history_sha256"] = hashlib.sha256(exported_history).hexdigest()
    _atomic_write(state_path, _serialize_state(state))
    _atomic_write(history_path, exported_history, MAX_HISTORY_BYTES)
    return summary


def _validate_state_records(records: list[dict[str, Any]]) -> None:
    if len(records) > MAX_HISTORY_ROWS:
        raise ValidationError("history row limit exceeded")
    identities: set[tuple[str, ...]] = set()
    for record in records:
        _validate_state_record(record)
        identity = tuple(record[name] for name in
                         ("version", "race", "opponent", "map", "strategy"))
        if identity in identities:
            raise ValidationError("validator state contains duplicate history rows")
        identities.add(identity)


def _serialize_history(records: list[dict[str, Any]]) -> str:
    lines = [HISTORY_HEADER, "# version,race,opponent,map,strategy,wins,losses"]
    total_bytes = sum(len(line.encode("utf-8")) + 1 for line in lines)
    for record in sorted(records, key=lambda row: tuple(row[name] for name in
                     ("version", "race", "opponent", "map", "strategy"))):
        line = ",".join(str(record[name]) for name in
                        ("version", "race", "opponent", "map", "strategy", "wins", "losses"))
        size = len(line.encode("utf-8")) + 1
        if size > MAX_HISTORY_LINE_BYTES:
            raise ValidationError("history row exceeds runtime line limit")
        total_bytes += size
        if total_bytes > MAX_HISTORY_BYTES:
            raise ValidationError("exported history exceeds runtime byte limit")
        lines.append(line)
    return "\n".join(lines) + "\n"


def _atomic_write(path: Path, data: bytes, maximum_bytes: int = MAX_FILE_BYTES) -> None:
    if len(data) > maximum_bytes:
        raise ValidationError(f"output {path} exceeds byte limit")
    path.parent.mkdir(parents=True, exist_ok=True)
    temp_name = ""
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", delete=False) as output:
            temp_name = output.name
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temp_name, path)
    finally:
        if temp_name and os.path.exists(temp_name):
            os.unlink(temp_name)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--log", required=True, type=Path, help="Protodd.log containing PvT selection/result events")
    parser.add_argument("--trusted-results", required=True, type=Path,
                        help="independent validator's trusted result ledger")
    parser.add_argument("--state", required=True, type=Path, help="deduplication state JSON (persistent)")
    parser.add_argument("--history", required=True, type=Path, help="cumulative v1 CSV consumed by Protodd")
    args = parser.parse_args()
    try:
        summary = validate_campaign(args.log, args.trusted_results, args.state, args.history)
    except (OSError, csv.Error, ValidationError) as exc:
        parser.error(str(exc))
    print(json.dumps(summary, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
