"""Resumable command/metadata audit of raw human replays, NOT state extraction.

The output deliberately cannot be consumed by training.prepare. Parsing commands
does not establish command acceptance, legal observations, or playback fidelity.
Raw downloads are read only; completed files are snapshotted at invocation time.
"""
import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import time

AUDIT_VERSION = 1
RACES = {0: "Z", 1: "T", 2: "P"}
MACRO_REQUESTS = {"Train", "Build", "Unit Morph", "Building Morph", "Tech", "Upgrade"}


def digest(data):
    return hashlib.sha256(data).hexdigest()


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=True) + "\n", encoding="utf8")
    temporary.replace(path)


def source_identity(player, players, source):
    """Attach claims only when a catalog name/race identifies a unique slot.

    Names and source pro tags are provenance, not verified identity. Opponent MMR
    must never be copied from the catalog's primary player.
    """
    claims = []
    for prefix in ("", "opponent"):
        def field(name):
            return source.get(prefix + name[0].upper() + name[1:] if prefix else name)
        matches = [p for p in players if p["Name"] == field("toon")
                   and RACES.get(p["Race"]["ID"]) == field("race")]
        if len(matches) == 1 and matches[0]["ID"] == player["ID"]:
            claims.append({"catalog_side": prefix or "primary", "toon": field("toon"),
                           "aurora_id": field("auroraId"), "pro_id_claim": field("proId"),
                           "pro_name_claim": field("proName"), "mmr_claim": field("mmr"),
                           "identity_verified": False})
    return claims


def summarize(parsed, source, expected_matchup):
    header = parsed["Header"]
    commands = parsed["Commands"]
    cmds = commands["Cmds"] or []
    errors = commands.get("ParseErrCmds") or []
    players = [p for p in header["Players"] if p["Type"]["ID"] == 2 and not p.get("Observer")]
    races = [RACES.get(p["Race"]["ID"], "?") for p in players]
    reasons = []
    if len(players) != 2 or len({p["Team"] for p in players}) != 2:
        reasons.append("not_two_opposing_humans")
    if sorted(races) != sorted(expected_matchup.split("v")):
        reasons.append("catalog_matchup_mismatch")
    if header["Engine"]["ID"] != 1 or header["Speed"]["ID"] != 6:
        reasons.append("unsupported_engine_or_speed")
    if header["Type"]["ID"] not in (2, 15):
        reasons.append("not_melee_or_top_vs_bottom")
    if errors:
        reasons.append("command_parse_errors")
    if not cmds:
        reasons.append("empty_commands")
    frames = [c["Frame"] for c in cmds]
    if frames and (min(frames) < 0 or max(frames) > header["Frames"] or frames != sorted(frames)):
        reasons.append("invalid_command_timing")
    map_hash = parsed.get("Custom", {}).get("MapDataHash")
    if not isinstance(map_hash, str) or len(map_hash) != 64:
        reasons.append("missing_map_hash")
    counts = Counter(str(c["Type"]["ID"]) for c in cmds)
    summaries = []
    for player, race in zip(players, races):
        requests = Counter(c["Type"]["Name"] for c in cmds
                           if c["PlayerID"] == player["ID"] and c["Type"]["Name"] in MACRO_REQUESTS)
        summaries.append({"player_id": player["ID"], "slot_id": player["SlotID"],
                          "name": player["Name"], "race": race, "team": player["Team"],
                          "source_claims": source_identity(player, players, source),
                          "macro_request_counts": dict(requests)})
    unit_limit = parsed.get("Limits", {}).get("units")
    blockers = ["state_extractor_missing", "perspective_audit_missing", "action_acceptance_audit_missing"]
    if header["Version"] != "-1.16":
        blockers.append("modern_replay_requires_compatible_playback")
    if unit_limit is not None and unit_limit > 1700:
        blockers.append("unit_limit_exceeds_legacy_engine")
    return {"parse_status": "quarantined" if reasons else "parsed", "quarantine_reasons": reasons,
            "version": header["Version"], "frames": header["Frames"],
            "start_time": header["StartTime"], "map_name": header["Map"], "map_sha256": map_hash,
            "game_type": header["Type"], "unit_limit": unit_limit, "players": summaries,
            "command_count": len(cmds), "command_type_counts": dict(counts),
            "command_parse_error_count": len(errors), "first_parse_errors": errors[:3],
            # This hash is diagnostic only: alternate recordings can differ in
            # chat/observer commands. It is NOT a canonical duplicate-group key.
            "parsed_commands_sha256": digest(json.dumps(cmds, sort_keys=True, separators=(",", ":")).encode()),
            "training_ready": False, "training_blockers": blockers,
            "source_record": source}


def audit_one(path, root, source, parser, timeout):
    result = {"path": path.relative_to(root).as_posix(), "match_id": path.stem,
              "matchup": path.parent.name, "audited_at": utc_now(), "training_ready": False}
    try:
        before = path.stat()
        data = path.read_bytes()
        after = path.stat()
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise ValueError("replay changed while being read; retry after download completes")
        result.update(sha256=digest(data), size=len(data), mtime_ns=after.st_mtime_ns)
        completed = subprocess.run([str(parser), "-stdin", "-cmds", "-map", "-computed=false",
                                    "-mapDataHash", "sha256", "-indent=false"], input=data,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout,
                                   check=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        result.update(summarize(json.loads(completed.stdout), source, path.parent.name))
        if completed.stderr:
            result["parser_warnings"] = completed.stderr.decode("utf8", errors="replace")[:2000]
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
        result.update(parse_status="failed", error=str(error)[:2000])
        if isinstance(error, subprocess.CalledProcessError):
            result["parser_stderr"] = error.stderr.decode("utf8", errors="replace")[:2000]
    return result


def report(connection, snapshot_count, pending_count, started, complete):
    summary = {"audit_version": AUDIT_VERSION, "updated_at": utc_now(), "complete": complete,
               "snapshot_replays": snapshot_count, "scheduled_this_invocation": pending_count,
               "elapsed_seconds_this_invocation": round(time.monotonic() - started, 2),
               "training_ready_replays": 0, "training_started": False,
               "next_required_stage": "compatible playback and validated player-perspective state extraction"}
    for key, field in (("parse_status", "status"), ("matchups", "matchup")):
        summary[key] = dict(connection.execute(f"SELECT {field}, count(*) FROM replays GROUP BY {field}"))
    formats, limits, reasons, identities = Counter(), Counter(), Counter(), Counter()
    for (payload,) in connection.execute("SELECT detail FROM replays"):
        detail = json.loads(payload)
        formats[detail.get("version", "unknown")] += 1
        limits[str(detail.get("unit_limit"))] += 1
        reasons.update(detail.get("quarantine_reasons", []))
        for player in detail.get("players", []):
            if player["race"] == "P":
                identities["protoss_perspectives"] += 1
                if player["source_claims"]:
                    identities["protoss_catalog_name_matches"] += 1
                if any(c["pro_id_claim"] or c["pro_name_claim"] for c in player["source_claims"]):
                    identities["protoss_with_source_pro_claim_not_verified"] += 1
    summary.update(formats=dict(formats), unit_limits=dict(limits), quarantine_reasons=dict(reasons),
                   identity_claims=dict(identities),
                   exact_duplicate_files=connection.execute(
                       "SELECT coalesce(sum(n-1),0) FROM (SELECT count(*) n FROM replays "
                       "WHERE sha256 IS NOT NULL GROUP BY sha256 HAVING count(*)>1)").fetchone()[0])
    return summary


def run(args):
    root, output, parser = args.root.resolve(), args.output.resolve(), args.parser.resolve()
    if args.workers < 1 or args.workers > 8 or args.limit_per_matchup < 0 or args.timeout <= 0:
        raise ValueError("workers must be 1-8; limit nonnegative; timeout positive")
    version = subprocess.run([str(parser), "-version"], check=True, capture_output=True,
                             timeout=10).stdout.decode("utf8").strip()
    config = {"audit_version": AUDIT_VERSION, "root": str(root), "parser_sha256": digest(parser.read_bytes()),
              "parser_version": version, "auditor_sha256": digest(Path(__file__).read_bytes()),
              "matchups": sorted(args.matchups)}
    if output.exists():
        if not args.resume:
            raise ValueError("output exists; use --resume for a compatible audit")
        previous = json.loads((output / "config.json").read_text(encoding="utf8"))
        if previous != config:
            raise ValueError("audit source/parser/config changed; use a new output directory")
    else:
        output.mkdir(parents=True)
        write_json(output / "config.json", config)
    catalog, files = {}, []
    for matchup in args.matchups:
        directory = root / matchup
        with (directory / "catalog.jsonl").open(encoding="utf8") as stream:
            for line in stream:
                if line.strip():
                    entry = json.loads(line)
                    catalog[(matchup, entry["matchId"])] = entry
        selected = sorted(directory.glob("*.rep"))
        files.extend(selected[:args.limit_per_matchup] if args.limit_per_matchup else selected)
    connection = sqlite3.connect(output / "audit.sqlite")
    try:
        connection.execute("PRAGMA journal_mode=WAL")
        connection.execute("CREATE TABLE IF NOT EXISTS replays (path TEXT PRIMARY KEY, sha256 TEXT, "
                           "size INTEGER, mtime_ns INTEGER, matchup TEXT, status TEXT, detail TEXT NOT NULL)")
        connection.execute("CREATE INDEX IF NOT EXISTS replay_hash ON replays(sha256)")
        previous = {row[0]: row[1:] for row in connection.execute("SELECT path,size,mtime_ns,status FROM replays")}
        pending = []
        for path in files:
            stat = path.stat()
            saved = previous.get(path.relative_to(root).as_posix())
            if not saved or saved[:2] != (stat.st_size, stat.st_mtime_ns) or saved[2] == "failed":
                pending.append(path)
        started = time.monotonic()
        write_json(output / "summary.json", report(connection, len(files), len(pending), started, False))
        print(f"Auditing {len(pending):,} of {len(files):,} completed replay files; {args.workers} workers", flush=True)
        # Executor.map eagerly queues lightweight paths; at most `workers` parser
        # outputs are in flight. Only the coordinator writes SQLite.
        with ThreadPoolExecutor(max_workers=args.workers) as pool:
            def process(path):
                return audit_one(path, root, catalog.get((path.parent.name, path.stem), {}), parser, args.timeout)
            for index, result in enumerate(pool.map(process, pending), 1):
                connection.execute("INSERT OR REPLACE INTO replays VALUES(?,?,?,?,?,?,?)",
                                   (result["path"], result.get("sha256"), result.get("size"), result.get("mtime_ns"),
                                    result["matchup"], result["parse_status"], json.dumps(result, ensure_ascii=True)))
                if index % 100 == 0:
                    connection.commit()
                if index % 1000 == 0:
                    write_json(output / "summary.json", report(connection, len(files), len(pending), started, False))
                    print(f"Parsed {index:,}/{len(pending):,} in {time.monotonic()-started:.1f}s", flush=True)
        connection.commit()
        final = report(connection, len(files), len(pending), started, True)
        write_json(output / "summary.json", final)
        print(json.dumps(final, ensure_ascii=True, indent=2), flush=True)
    finally:
        connection.close()


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--root", type=Path, required=True)
    cli.add_argument("--parser", type=Path, required=True, help="Pinned screp executable")
    cli.add_argument("--output", type=Path, required=True)
    cli.add_argument("--matchups", nargs="+", choices=["PvP", "TvP", "ZvP", "TvT", "ZvT", "ZvZ"], default=["PvP", "TvP", "ZvP"])
    cli.add_argument("--workers", type=int, default=4)
    cli.add_argument("--limit-per-matchup", type=int, default=0, help="Pilot cap; 0 audits all completed files")
    cli.add_argument("--timeout", type=float, default=30)
    cli.add_argument("--resume", action="store_true")
    args = cli.parse_args()
    try:
        run(args)
    except (OSError, ValueError, sqlite3.Error, subprocess.SubprocessError) as error:
        cli.exit(1, f"replay audit failed: {error}\n")


if __name__ == "__main__":
    main()
