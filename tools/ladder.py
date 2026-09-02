#!/usr/bin/env python3
"""Prepare reproducible AstraBot ladders and analyze tournament results.

The match executor is davechurchill/StarcraftAITournamentManager. This tool owns
the reproducible schedule, private bot vault, run manifest, and statistical
reporting around that executor. It intentionally uses only the Python standard
library so it can run on a clean Windows tournament machine.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import html
import json
import math
import re
import shutil
import statistics
import subprocess
import sys
from collections import Counter, defaultdict
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable, Sequence


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_CONFIG = REPO_ROOT / "ladder" / "ladder.local.json"
PRIVATE_LADDER_PARTS = {"bots", "manager", "maps", "runs", "reports"}
END_TYPE_PRECEDENCE = {
    "NO_REPORT": 0,
    "GAME_STATE_NOT_UPDATED_60S_BOTH_BOTS": 1,
    "STARCRAFT_NEVER_DETECTED": 2,
    "GAME_STATE_NEVER_DETECTED": 3,
    "STARCRAFT_CRASH": 4,
    "GAME_STATE_NOT_UPDATED_60S": 5,
    "NORMAL": 6,
}


class LadderError(RuntimeError):
    """A user-actionable ladder configuration or data error."""


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat().replace("+00:00", "Z")


def load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except FileNotFoundError as error:
        raise LadderError(f"File not found: {path}") from error
    except json.JSONDecodeError as error:
        raise LadderError(f"Invalid JSON in {path}: {error}") from error
    if not isinstance(value, dict):
        raise LadderError(f"Expected a JSON object in {path}")
    return value


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def resolve_from(config_path: Path, value: str) -> Path:
    path = Path(value)
    return path.resolve() if path.is_absolute() else (config_path.parent / path).resolve()


def git_output(*args: str) -> str:
    process = subprocess.run(
        ["git", "-C", str(REPO_ROOT), *args],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if process.returncode != 0:
        raise LadderError(process.stderr.strip() or f"git {' '.join(args)} failed")
    return process.stdout.strip()


def directory_digest(path: Path) -> str:
    digest = hashlib.sha256()
    if path.is_file():
        digest.update(path.read_bytes())
        return digest.hexdigest()
    for item in sorted((entry for entry in path.rglob("*") if entry.is_file()), key=lambda p: p.as_posix().lower()):
        digest.update(item.relative_to(path).as_posix().encode("utf-8"))
        digest.update(b"\0")
        with item.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    return digest.hexdigest()


def validate_bot_name(name: str) -> None:
    if not re.fullmatch(r"[A-Za-z0-9._-]+", name) or name in {".", ".."}:
        raise LadderError(f"Unsafe bot name: {name}")


def validate_bot(bot: dict[str, Any], root: Path) -> None:
    required = ("name", "race", "type", "bwapi_version")
    missing = [field for field in required if not bot.get(field)]
    if missing:
        raise LadderError(f"Bot entry is missing: {', '.join(missing)}")
    validate_bot_name(str(bot["name"]))
    if bot["race"] not in {"Protoss", "Terran", "Zerg", "Random"}:
        raise LadderError(f"Invalid race for {bot['name']}: {bot['race']}")
    if bot["type"] not in {"dll", "proxy"}:
        raise LadderError(f"Invalid bot type for {bot['name']}: {bot['type']}")
    ai = root / "AI"
    dll = next((item for item in ai.glob("*.dll") if item.name.lower() == f"{bot['name']}.dll".lower()), None)
    if dll is None:
        raise LadderError(f"{bot['name']} requires AI/{bot['name']}.dll under {root}")
    if bot["type"] == "proxy" and not (ai / "run_proxy.bat").is_file():
        raise LadderError(f"Proxy bot {bot['name']} requires AI/run_proxy.bat")


def audit_privacy() -> list[str]:
    tracked = git_output("ls-files").splitlines()
    violations = []
    for raw in tracked:
        normalized = raw.replace("\\", "/")
        parts = normalized.split("/")
        if len(parts) >= 2 and parts[0] == "ladder" and parts[1] in PRIVATE_LADDER_PARTS:
            violations.append(normalized)
        if normalized == "ladder/ladder.local.json":
            violations.append(normalized)
    return sorted(set(violations))


def command_init(args: argparse.Namespace) -> None:
    ladder = REPO_ROOT / "ladder"
    for name in sorted(PRIVATE_LADDER_PARTS):
        (ladder / name).mkdir(parents=True, exist_ok=True)
    destination = Path(args.config).resolve()
    if not destination.exists():
        shutil.copy2(ladder / "ladder.example.json", destination)
        created = f"Created {destination}"
    else:
        created = f"Kept existing {destination}"
    probe = ladder / "bots" / ".privacy-probe"
    process = subprocess.run(
        ["git", "-C", str(REPO_ROOT), "check-ignore", "-q", str(probe)], check=False
    )
    if process.returncode != 0:
        raise LadderError("ladder/bots is not ignored by Git; refusing to initialize the bot vault")
    violations = audit_privacy()
    if violations:
        raise LadderError("Private ladder files are already tracked: " + ", ".join(violations))
    print(created)
    print("Private ladder directories are present and verified as Git-ignored.")


def command_audit(_: argparse.Namespace) -> None:
    violations = audit_privacy()
    if violations:
        raise LadderError("Private ladder files are tracked: " + ", ".join(violations))
    probes = [
        REPO_ROOT / "ladder" / name / ".privacy-probe" for name in PRIVATE_LADDER_PARTS
    ] + [DEFAULT_CONFIG]
    for probe in probes:
        process = subprocess.run(
            ["git", "-C", str(REPO_ROOT), "check-ignore", "-q", str(probe)], check=False
        )
        if process.returncode != 0:
            raise LadderError(f"Private path is not ignored: {probe}")
    print("Privacy audit passed: no local ladder assets are tracked and all vault paths are ignored.")


def normalize_bot_source(source: Path, name: str) -> Path:
    if source.is_file():
        if source.suffix.lower() != ".dll":
            raise LadderError("A bot source file must be a .dll")
        return source
    if not source.is_dir():
        raise LadderError(f"Bot source does not exist: {source}")
    if (source / "AI").is_dir():
        return source
    if source.name.lower() == "ai":
        return source
    if any(item.name.lower() == f"{name}.dll".lower() for item in source.glob("*.dll")):
        return source
    raise LadderError("Bot source must be a DLL, an AI directory, or a directory containing AI/")


def command_add_bot(args: argparse.Namespace) -> None:
    config_path = Path(args.config).resolve()
    config = load_json(config_path)
    validate_bot_name(args.name)
    opponents = config.setdefault("opponents", [])
    if any(item.get("name", "").lower() == args.name.lower() for item in opponents):
        raise LadderError(f"{args.name} is already present in {config_path}")
    source = normalize_bot_source(Path(args.source).resolve(), args.name)
    destination = REPO_ROOT / "ladder" / "bots" / args.name
    if destination.exists():
        raise LadderError(f"Bot already exists: {destination}; remove it explicitly before replacing it")
    (destination / "AI").mkdir(parents=True)
    if source.is_file():
        shutil.copy2(source, destination / "AI" / f"{args.name}.dll")
    elif source.name.lower() == "ai" or not (source / "AI").is_dir():
        shutil.copytree(source, destination / "AI", dirs_exist_ok=True)
    else:
        shutil.copytree(source, destination, dirs_exist_ok=True)
    (destination / "read").mkdir(exist_ok=True)
    (destination / "write").mkdir(exist_ok=True)
    bot = {
        "name": args.name,
        "race": args.race,
        "type": args.bot_type,
        "bwapi_version": args.bwapi_version,
        "directory": str(destination.relative_to(config_path.parent)).replace("\\", "/"),
        "source_url": args.source_url or "",
        "version": args.version or "unrecorded",
    }
    validate_bot(bot, destination)
    provenance = {
        "name": args.name,
        "source_url": args.source_url or None,
        "version": args.version or None,
        "license": args.license or None,
        "imported_utc": utc_now(),
        "sha256": directory_digest(destination),
    }
    write_json(destination / ".astra-ladder.json", provenance)
    opponents.append(bot)
    write_json(config_path, config)
    print(f"Imported {args.name} into ignored vault: {destination}")
    print(f"Recorded source SHA-256: {provenance['sha256']}")


def require_config(config_path: Path) -> dict[str, Any]:
    config = load_json(config_path)
    if not isinstance(config.get("our_bot"), dict):
        raise LadderError("Configuration requires an our_bot object")
    if not isinstance(config.get("opponents"), list) or not config["opponents"]:
        raise LadderError("Add at least one opponent with the add-bot command")
    if not isinstance(config.get("maps"), list) or not config["maps"]:
        raise LadderError("Configuration requires at least one map")
    rounds = config.get("rounds", 0)
    if not isinstance(rounds, int) or rounds < 1:
        raise LadderError("rounds must be a positive integer")
    return config


def make_schedule(our_name: str, opponents: Sequence[dict[str, Any]], maps: Sequence[str], rounds: int) -> list[dict[str, Any]]:
    games: list[dict[str, Any]] = []
    game_id = 0
    round_id = 0
    for repetition in range(rounds):
        for map_name in maps:
            for opponent_index, opponent in enumerate(opponents):
                flip = (repetition + opponent_index) % 2 == 1
                home, away = ((opponent["name"], our_name) if flip else (our_name, opponent["name"]))
                games.append(
                    {
                        "gameID": game_id,
                        "roundID": round_id,
                        "homeBot": home,
                        "awayBot": away,
                        "map": map_name,
                    }
                )
                game_id += 1
            round_id += 1
    return games


def stage_bot(bot: dict[str, Any], source: Path, bots_root: Path) -> dict[str, Any]:
    validate_bot(bot, source)
    destination = bots_root / bot["name"]
    shutil.copytree(source, destination)
    return {
        "name": bot["name"],
        "race": bot["race"],
        "type": bot["type"],
        "bwapi_version": bot["bwapi_version"],
        "source_url": bot.get("source_url", ""),
        "version": bot.get("version", "unrecorded"),
        "sha256": directory_digest(source),
    }


def command_prepare(args: argparse.Namespace) -> None:
    config_path = Path(args.config).resolve()
    config = require_config(config_path)
    label = args.label or datetime.now().strftime("%Y%m%d-%H%M%S")
    if not re.fullmatch(r"[A-Za-z0-9._-]+", label):
        raise LadderError("Run label may contain only letters, numbers, dots, underscores, and hyphens")
    run_root = REPO_ROOT / "ladder" / "runs" / label
    if run_root.exists():
        raise LadderError(f"Run already exists: {run_root}")

    our_bot = dict(config["our_bot"])
    validate_bot_name(str(our_bot.get("name", "")))
    artifact = resolve_from(config_path, str(our_bot.get("artifact", "")))
    if not artifact.is_file():
        raise LadderError(f"AstraBot artifact not found: {artifact}")
    opponent_sources = []
    for opponent in config["opponents"]:
        source = resolve_from(config_path, str(opponent.get("directory", "")))
        validate_bot(opponent, source)
        opponent_sources.append((opponent, source))
    archive = None
    archive_value = config.get("maps_archive")
    if archive_value:
        archive = resolve_from(config_path, str(archive_value))
        if not archive.is_file():
            raise LadderError(f"Map archive not found: {archive}")

    manager_value = str(config.get("tournament_manager", "manager"))
    manager = resolve_from(config_path, manager_value)
    tournament = run_root / "tournament"
    if manager.is_dir() and (manager / "server").is_dir():
        shutil.copytree(manager, tournament)
    else:
        (tournament / "server").mkdir(parents=True)
        print(f"Warning: Tournament Manager not found at {manager}; generated a server overlay only.")
    server = tournament / "server"
    bots_root = server / "bots"
    if bots_root.exists():
        shutil.rmtree(bots_root)
    bots_root.mkdir(parents=True)

    our_source = run_root / "_our_bot_stage"
    (our_source / "AI").mkdir(parents=True)
    shutil.copy2(artifact, our_source / "AI" / f"{our_bot['name']}.dll")
    (our_source / "read").mkdir()
    (our_source / "write").mkdir()
    validate_bot(our_bot, our_source)
    staged = [stage_bot(our_bot, our_source, bots_root)]
    shutil.rmtree(our_source)

    for opponent, source in opponent_sources:
        staged.append(stage_bot(opponent, source, bots_root))

    required = server / "required"
    required.mkdir(exist_ok=True)
    maps_file = "maps.zip"
    if archive is not None:
        maps_file = archive.name
        shutil.copy2(archive, required / maps_file)

    timeouts = config.get("timeout_limits", [])
    settings = {
        "bots": [
            {
                "BotName": bot["name"],
                "Race": bot["race"],
                "BotType": bot["type"],
                "BWAPIVersion": bot["bwapi_version"],
            }
            for bot in staged
        ],
        "maps": config["maps"],
        "mapsFile": maps_file,
        "gamesListFile": "games.txt",
        "resultsFile": "results.txt",
        "detailedResults": True,
        "serverPort": int(config.get("server_port", 1337)),
        "clearResults": "ask",
        "startGamesSimultaneously": False,
        "tournamentType": "1VsAll",
        "lobbyGameSpeed": "Normal",
        "enableBotFileIO": True,
        "ladderMode": False,
        "writeCrashLogs": True,
        "crashLogDir": "crash_logs",
        "excludeFromResults": [],
        "tournamentModuleSettings": {
            "localSpeed": 0,
            "frameSkip": 256,
            "gameFrameLimit": int(config.get("frame_limit", 86400)),
            "timeoutLimits": [
                {"timeInMS": int(item["time_ms"]), "frameCount": int(item["frame_count"])}
                for item in timeouts
            ],
            "drawBotNames": True,
            "drawTournamentInfo": True,
            "drawUnitInfo": False,
        },
    }
    write_json(server / "server_settings.json", settings)
    games = make_schedule(our_bot["name"], config["opponents"], config["maps"], config["rounds"])
    (server / "games.txt").write_text(
        "".join(json.dumps(game, separators=(",", ":")) + "\n" for game in games), encoding="utf-8"
    )
    commit = git_output("rev-parse", "HEAD")
    dirty = bool(git_output("status", "--porcelain"))
    manifest = {
        "schema": 1,
        "label": label,
        "created_utc": utc_now(),
        "our_bot": our_bot["name"],
        "git_commit": commit,
        "git_dirty": dirty,
        "artifact": str(artifact),
        "artifact_sha256": directory_digest(artifact),
        "bots": staged,
        "maps": config["maps"],
        "rounds": config["rounds"],
        "scheduled_games": len(games),
        "frame_limit": int(config.get("frame_limit", 86400)),
        "timeout_limits": timeouts,
        "settings_sha256": directory_digest(server / "server_settings.json"),
    }
    write_json(run_root / "manifest.json", manifest)
    (run_root / "RUN.md").write_text(
        "# Ladder run: " + label + "\n\n"
        "This entire run is Git-ignored. Start `tournament/server/run_server.bat`, connect two configured "
        "Tournament Manager clients, then analyze `tournament/server/results.txt`.\n\n"
        f"Scheduled games: {len(games)}\n\n"
        f"Git commit: `{commit}`" + (" (dirty worktree)\n" if dirty else "\n"),
        encoding="utf-8",
    )
    print(f"Prepared {len(games)} games in {run_root}")
    if dirty:
        print("Warning: the run snapshots a dirty worktree; use a clean commit for a defensible baseline.")
    print(f"Server directory: {server}")


def read_result_objects(paths: Sequence[Path]) -> list[dict[str, Any]]:
    objects: list[dict[str, Any]] = []
    for path in paths:
        text = path.read_text(encoding="utf-8", errors="replace").strip()
        detailed = re.search(r"var\s+detailedResults\s*=\s*(\[.*\])\s*;?\s*$", text, re.DOTALL)
        if detailed:
            parsed = json.loads(detailed.group(1))
            objects.extend(item for item in parsed if isinstance(item, dict))
            continue
        try:
            parsed = json.loads(text)
        except json.JSONDecodeError:
            for line_number, line in enumerate(text.splitlines(), 1):
                if not line.strip():
                    continue
                try:
                    item = json.loads(line)
                except json.JSONDecodeError as error:
                    raise LadderError(f"Invalid JSON at {path}:{line_number}: {error}") from error
                if isinstance(item, dict):
                    objects.append(item)
        else:
            if isinstance(parsed, list):
                objects.extend(item for item in parsed if isinstance(item, dict))
            elif isinstance(parsed, dict):
                objects.append(parsed)
    return objects


def duration_to_frames(value: str) -> int:
    try:
        hours, minutes, seconds = (int(part) for part in value.split(":"))
    except (TypeError, ValueError):
        return 0
    return (hours * 3600 + minutes * 60 + seconds) * 24


def normalize_detailed(item: dict[str, Any], our_bot: str) -> dict[str, Any]:
    bots = [str(value) for value in item.get("bots", [])]
    winner_index = item.get("winner", -1)
    winner = bots[winner_index] if isinstance(winner_index, int) and 0 <= winner_index < len(bots) else None
    opponent = next((bot for bot in bots if bot != our_bot), "unknown")
    crash_index = item.get("crash", -1)
    timeout_index = item.get("timeout", -1)
    frames = int(item.get("finalFrame", 0) or 0) or duration_to_frames(str(item.get("duration", "")))
    return {
        "game_id": int(item.get("gameID", -1)),
        "round": int(item.get("round", item.get("roundID", -1))),
        "opponent": opponent,
        "map": str(item.get("map", "unknown")),
        "won": winner == our_bot if winner is not None else None,
        "winner": winner,
        "frames": frames,
        "end_type": str(item.get("gameEndType", "NORMAL")),
        "our_crash": isinstance(crash_index, int) and 0 <= crash_index < len(bots) and bots[crash_index] == our_bot,
        "opponent_crash": isinstance(crash_index, int) and 0 <= crash_index < len(bots) and bots[crash_index] != our_bot,
        "our_timeout": isinstance(timeout_index, int) and 0 <= timeout_index < len(bots) and bots[timeout_index] == our_bot,
        "opponent_timeout": isinstance(timeout_index, int) and 0 <= timeout_index < len(bots) and bots[timeout_index] != our_bot,
        "game_timeout": bool(item.get("gameTimeout", False)),
        "complete": str(item.get("gameEndType", "NORMAL")) != "NO_REPORT",
    }


def merge_raw_reports(items: Sequence[dict[str, Any]], our_bot: str, timeout_limits: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    grouped: dict[int, list[dict[str, Any]]] = defaultdict(list)
    for item in items:
        grouped[int(item.get("gameID", -1))].append(item)
    records = []
    for game_id, reports in sorted(grouped.items()):
        first = reports[0]
        bots: list[str] = []
        for report in reports:
            for key in ("reportingBot", "opponentBot"):
                name = str(report.get(key, ""))
                if name and name not in bots:
                    bots.append(name)
        winner = next((str(report["reportingBot"]) for report in reports if report.get("won")), None)
        crash_bot = next((str(report["reportingBot"]) for report in reports if report.get("crash")), None)
        timeout_bot = None
        for report in reports:
            timers = report.get("timers", [])
            for index, limit in enumerate(timeout_limits):
                if index >= len(timers):
                    continue
                timer = timers[index]
                count = int(timer.get("frameCount", -1)) if isinstance(timer, dict) else int(timer)
                if count >= int(limit.get("frame_count", limit.get("frameCount", 0))):
                    timeout_bot = str(report.get("reportingBot"))
                    break
        failed_bot = crash_bot or timeout_bot
        if failed_bot and len(bots) == 2:
            winner = next(bot for bot in bots if bot != failed_bot)
        end_types = [str(report.get("gameEndType", "NORMAL")) for report in reports]
        if end_types.count("GAME_STATE_NOT_UPDATED_60S") >= 2:
            end_type = "GAME_STATE_NOT_UPDATED_60S_BOTH_BOTS"
        else:
            end_type = min(end_types, key=lambda value: END_TYPE_PRECEDENCE.get(value, 99))
        opponent = next((bot for bot in bots if bot != our_bot), "unknown")
        records.append(
            {
                "game_id": game_id,
                "round": int(first.get("round", -1)),
                "opponent": opponent,
                "map": str(first.get("map", "unknown")),
                "won": winner == our_bot if winner is not None else None,
                "winner": winner,
                "frames": max(int(report.get("finalFrame", 0) or 0) for report in reports),
                "end_type": end_type if len(reports) >= 2 else "NO_REPORT",
                "our_crash": crash_bot == our_bot,
                "opponent_crash": crash_bot not in {None, our_bot},
                "our_timeout": timeout_bot == our_bot,
                "opponent_timeout": timeout_bot not in {None, our_bot},
                "game_timeout": any(bool(report.get("gameTimeout")) for report in reports),
                "complete": len({str(report.get("reportingBot")) for report in reports}) >= 2,
            }
        )
    return records


def parse_results(paths: Sequence[Path], our_bot: str, timeout_limits: Sequence[dict[str, Any]]) -> list[dict[str, Any]]:
    objects = read_result_objects(paths)
    if not objects:
        return []
    if all("bots" in item and "winner" in item for item in objects):
        return [normalize_detailed(item, our_bot) for item in objects]
    if all("reportingBot" in item for item in objects):
        return merge_raw_reports(objects, our_bot, timeout_limits)
    if all("our_bot" in item and "won" in item for item in objects):
        return objects
    raise LadderError("Unrecognized result schema; expected Tournament Manager raw or detailed JSON")


def wilson(wins: int, games: int, z: float = 1.959963984540054) -> list[float]:
    if games <= 0:
        return [0.0, 1.0]
    rate = wins / games
    denominator = 1.0 + z * z / games
    center = (rate + z * z / (2.0 * games)) / denominator
    margin = z * math.sqrt(rate * (1.0 - rate) / games + z * z / (4.0 * games * games)) / denominator
    return [max(0.0, center - margin), min(1.0, center + margin)]


def indication(wins: int, games: int) -> str:
    low, high = wilson(wins, games)
    evidence = "insufficient" if games < 30 else "directional" if games < 100 else "strong"
    direction = "winning" if low > 0.5 else "losing" if high < 0.5 else "uncertain"
    return f"{evidence}: {direction}"


def group_stats(records: Sequence[dict[str, Any]], key: str) -> dict[str, dict[str, Any]]:
    groups: dict[str, list[dict[str, Any]]] = defaultdict(list)
    for record in records:
        groups[str(record.get(key, "unknown"))].append(record)
    result: dict[str, dict[str, Any]] = {}
    for name, games in sorted(groups.items()):
        wins = sum(record["won"] is True for record in games)
        result[name] = {
            "games": len(games),
            "wins": wins,
            "losses": len(games) - wins,
            "win_rate": wins / len(games),
            "wilson_95": wilson(wins, len(games)),
            "indication": indication(wins, len(games)),
            "average_minutes": statistics.fmean(record["frames"] for record in games) / 1440.0,
            "our_crashes": sum(bool(record.get("our_crash")) for record in games),
            "our_timeouts": sum(bool(record.get("our_timeout")) for record in games),
        }
    return result


def summarize(records: Sequence[dict[str, Any]], our_bot: str, manifest: dict[str, Any] | None = None) -> dict[str, Any]:
    excluded = [record for record in records if not record.get("complete") or record.get("won") is None or record.get("frames", 0) <= 0 or record.get("end_type") == "GAME_STATE_NOT_UPDATED_60S_BOTH_BOTS"]
    scored = [record for record in records if record not in excluded]
    wins = sum(record["won"] is True for record in scored)
    scheduled = int((manifest or {}).get("scheduled_games", len(records)))
    received_ids = {int(record.get("game_id", -1)) for record in records}
    first_half = scored[: len(scored) // 2]
    second_half = scored[len(scored) // 2 :]
    races = {
        str(bot["name"]): str(bot.get("race", "unknown"))
        for bot in (manifest or {}).get("bots", [])
    }
    for record in scored:
        record["opponent_race"] = races.get(record["opponent"], "unknown")
    def rate(games: Sequence[dict[str, Any]]) -> float | None:
        return sum(record["won"] is True for record in games) / len(games) if games else None
    return {
        "schema": 1,
        "generated_utc": utc_now(),
        "our_bot": our_bot,
        "manifest": manifest,
        "summary": {
            "reported_games": len(records),
            "scored_games": len(scored),
            "excluded_incomplete_games": len(excluded),
            "missing_scheduled_games": max(0, scheduled - len(received_ids)),
            "wins": wins,
            "losses": len(scored) - wins,
            "win_rate": wins / len(scored) if scored else 0.0,
            "wilson_95": wilson(wins, len(scored)),
            "indication": indication(wins, len(scored)),
            "average_game_minutes": statistics.fmean(record["frames"] for record in scored) / 1440.0 if scored else 0.0,
            "our_crashes": sum(bool(record.get("our_crash")) for record in records),
            "our_timeouts": sum(bool(record.get("our_timeout")) for record in records),
            "game_timeouts": sum(bool(record.get("game_timeout")) for record in records),
            "first_half_win_rate": rate(first_half),
            "second_half_win_rate": rate(second_half),
            "latest_20_win_rate": rate(scored[-20:]),
        },
        "by_opponent": group_stats(scored, "opponent"),
        "by_race": group_stats(scored, "opponent_race"),
        "by_map": group_stats(scored, "map"),
        "end_types": dict(sorted(Counter(str(record.get("end_type", "unknown")) for record in records).items())),
        "games": list(records),
    }


def percent(value: float | None) -> str:
    return "n/a" if value is None else f"{100.0 * value:.1f}%"


def markdown_table(groups: dict[str, dict[str, Any]]) -> str:
    lines = ["| Group | W-L | Win rate | 95% interval | Signal | Crashes |", "|---|---:|---:|---:|---|---:|"]
    for name, item in sorted(groups.items(), key=lambda pair: (pair[1]["win_rate"], pair[0])):
        low, high = item["wilson_95"]
        lines.append(
            f"| {name.replace('|', '/')} | {item['wins']}-{item['losses']} | {percent(item['win_rate'])} | "
            f"{percent(low)} to {percent(high)} | {item['indication']} | {item['our_crashes']} |"
        )
    return "\n".join(lines)


def report_markdown(report: dict[str, Any]) -> str:
    summary = report["summary"]
    low, high = summary["wilson_95"]
    label = (report.get("manifest") or {}).get("label", "unlabelled")
    telemetry = report.get("astra_telemetry")
    telemetry_section = ""
    if telemetry:
        runtime = telemetry["runtime"]
        diagnostics = telemetry["diagnostics"]
        telemetry_section = f"""
## Bot diagnostics

- Runtime: {runtime['peak_frame_ms']:.2f} ms peak frame, {runtime['over_55ms']} frames over 55 ms, {runtime['over_1s']} over 1 s, {runtime['caught_errors']} caught errors.
- Decision snapshots: {diagnostics['supply_block_snapshots']} supply-blocked, {diagnostics['late_high_bank_snapshots']} late with 800+ minerals, {diagnostics['dangerous_fight_snapshots']} aggressive below a 0.85 fight ratio, {diagnostics['late_high_uncertainty_snapshots']} late with high enemy uncertainty.

The per-game values are in `telemetry-games.csv`; use them to open the matching replay and fix recurring failure modes.
"""
    return f"""# AstraBot ladder report: {label}

Generated {report['generated_utc']}.

## Verdict

**{summary['indication']}** - {summary['wins']}-{summary['losses']} over {summary['scored_games']} scored games, {percent(summary['win_rate'])} win rate (Wilson 95% CI {percent(low)} to {percent(high)}).

- Reliability: {summary['our_crashes']} Astra crashes, {summary['our_timeouts']} per-frame timeouts, {summary['game_timeouts']} game-length timeouts, {summary['excluded_incomplete_games']} incomplete/excluded.
- Coverage: {summary['reported_games']} result records, {summary['missing_scheduled_games']} scheduled games missing.
- Trend: first half {percent(summary['first_half_win_rate'])}, second half {percent(summary['second_half_win_rate'])}, latest 20 {percent(summary['latest_20_win_rate'])}.
- Average game length: {summary['average_game_minutes']:.1f} in-game minutes.
{telemetry_section}

## By opponent

{markdown_table(report['by_opponent'])}

## By race

{markdown_table(report['by_race'])}

## By map

{markdown_table(report['by_map'])}
"""


def report_html(report: dict[str, Any]) -> str:
    summary = report["summary"]
    low, high = summary["wilson_95"]
    def rows(groups: dict[str, dict[str, Any]]) -> str:
        output = []
        for name, item in sorted(groups.items(), key=lambda pair: (pair[1]["win_rate"], pair[0])):
            lo, hi = item["wilson_95"]
            width = max(1.0, item["win_rate"] * 100.0)
            output.append(
                "<tr><td>" + html.escape(name) + "</td><td>" + str(item["games"]) + "</td><td>" +
                percent(item["win_rate"]) + f"</td><td><span class='bar' style='width:{width:.1f}%'></span></td><td>" +
                percent(lo) + " to " + percent(hi) + "</td><td>" + html.escape(item["indication"]) + "</td></tr>"
            )
        return "".join(output)
    sections = "".join(
        f"<section><h2>By {title}</h2><table><thead><tr><th>{title.title()}</th><th>Games</th><th>Win rate</th><th></th><th>95% interval</th><th>Signal</th></tr></thead><tbody>{rows(report[key])}</tbody></table></section>"
        for title, key in (("opponent", "by_opponent"), ("race", "by_race"), ("map", "by_map"))
    )
    telemetry = report.get("astra_telemetry")
    diagnostic_section = ""
    if telemetry:
        runtime = telemetry["runtime"]
        diagnostics = telemetry["diagnostics"]
        diagnostic_section = f"""<section><h2>Bot diagnostics</h2><div class="cards"><div class="card">Peak frame<div class="big">{runtime['peak_frame_ms']:.2f} ms</div><div class="sub">{runtime['over_55ms']} over 55 ms · {runtime['caught_errors']} errors</div></div><div class="card">Supply blocks<div class="big">{diagnostics['supply_block_snapshots']}</div><div class="sub">decision snapshots</div></div><div class="card">Late high bank<div class="big">{diagnostics['late_high_bank_snapshots']}</div><div class="sub">800+ minerals after 5:00</div></div><div class="card">Bad fight entry<div class="big">{diagnostics['dangerous_fight_snapshots']}</div><div class="sub">aggressive under 0.85 ratio</div></div></div></section>"""
    return f"""<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width">
<title>AstraBot ladder report</title><style>
:root{{--bg:#0b1020;--panel:#151c32;--text:#edf2ff;--muted:#9daaca;--accent:#6ee7b7;--bad:#fb7185}}*{{box-sizing:border-box}}body{{margin:0;background:var(--bg);color:var(--text);font:15px system-ui,sans-serif}}main{{max-width:1120px;margin:auto;padding:32px}}h1{{font-size:32px;margin-bottom:6px}}.sub{{color:var(--muted)}}.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px;margin:24px 0}}.card,section{{background:var(--panel);border:1px solid #283352;border-radius:12px;padding:18px}}.big{{font-size:28px;font-weight:750;margin-top:8px}}section{{margin:16px 0;overflow:auto}}table{{width:100%;border-collapse:collapse}}th,td{{padding:10px;text-align:left;border-bottom:1px solid #283352;white-space:nowrap}}th{{color:var(--muted)}}.track{{width:130px}}.bar{{display:block;height:8px;max-width:130px;background:var(--accent);border-radius:10px}}@media(max-width:650px){{main{{padding:16px}}}}
</style></head><body><main><h1>AstraBot ladder report</h1><div class="sub">{html.escape(report['generated_utc'])} · {html.escape(summary['indication'])}</div>
<div class="cards"><div class="card">Win rate<div class="big">{percent(summary['win_rate'])}</div><div class="sub">95% CI {percent(low)} to {percent(high)}</div></div><div class="card">Record<div class="big">{summary['wins']}-{summary['losses']}</div><div class="sub">{summary['scored_games']} scored games</div></div><div class="card">Reliability<div class="big">{summary['our_crashes']} crashes</div><div class="sub">{summary['our_timeouts']} timeouts / {summary['excluded_incomplete_games']} incomplete</div></div><div class="card">Coverage<div class="big">{summary['reported_games']}</div><div class="sub">{summary['missing_scheduled_games']} scheduled games missing</div></div><div class="card">Latest 20<div class="big">{percent(summary['latest_20_win_rate'])}</div><div class="sub">First {percent(summary['first_half_win_rate'])} / second {percent(summary['second_half_win_rate'])}</div></div></div>{diagnostic_section}{sections}</main></body></html>"""


def write_games_csv(path: Path, games: Sequence[dict[str, Any]]) -> None:
    fields = ["game_id", "round", "opponent", "opponent_race", "map", "won", "winner", "frames", "end_type", "our_crash", "opponent_crash", "our_timeout", "opponent_timeout", "game_timeout", "complete"]
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(games)


def write_telemetry_csv(path: Path, games: Sequence[dict[str, Any]]) -> None:
    if not games:
        return
    fields = list(games[0])
    with path.open("w", newline="", encoding="utf-8") as target:
        writer = csv.DictWriter(target, fieldnames=fields, extrasaction="ignore")
        writer.writeheader()
        writer.writerows(games)


def command_report(args: argparse.Namespace) -> None:
    manifest = load_json(Path(args.manifest).resolve()) if args.manifest else None
    config = load_json(Path(args.config).resolve()) if args.config and Path(args.config).exists() else {}
    our_bot = args.our_bot or (manifest or {}).get("our_bot") or config.get("our_bot", {}).get("name", "AstraBot")
    timeouts = config.get("timeout_limits", (manifest or {}).get("timeout_limits", []))
    records = parse_results([Path(path).resolve() for path in args.results], our_bot, timeouts)
    report = summarize(records, our_bot, manifest)
    if args.astra_log:
        sys.path.insert(0, str(Path(__file__).resolve().parent))
        from log_analyzer import analyze  # pylint: disable=import-outside-toplevel
        lines = (
            line
            for raw_path in args.astra_log
            for line in Path(raw_path).read_text(encoding="utf-8", errors="replace").splitlines()
        )
        report["astra_telemetry"] = analyze(lines)
    output = Path(args.output).resolve() if args.output else REPO_ROOT / "ladder" / "reports" / datetime.now().strftime("%Y%m%d-%H%M%S")
    output.mkdir(parents=True, exist_ok=False)
    write_json(output / "report.json", report)
    (output / "report.md").write_text(report_markdown(report), encoding="utf-8")
    (output / "index.html").write_text(report_html(report), encoding="utf-8")
    write_games_csv(output / "games.csv", report["games"])
    if report.get("astra_telemetry"):
        write_telemetry_csv(output / "telemetry-games.csv", report["astra_telemetry"]["game_details"])
    print(report_markdown(report))
    print(f"Reports written to {output}")


def difference_interval(a_wins: int, a_games: int, b_wins: int, b_games: int) -> tuple[float, float, float]:
    if a_games <= 0 or b_games <= 0:
        return 0.0, -1.0, 1.0
    a = a_wins / a_games
    b = b_wins / b_games
    delta = b - a
    a_low, a_high = wilson(a_wins, a_games)
    b_low, b_high = wilson(b_wins, b_games)
    # Newcombe's score interval combines the two Wilson intervals and remains
    # well behaved for perfect 0% or 100% batches where a Wald interval fails.
    lower = delta - math.sqrt((b - b_low) ** 2 + (a_high - a) ** 2)
    upper = delta + math.sqrt((b_high - b) ** 2 + (a - a_low) ** 2)
    return delta, max(-1.0, lower), min(1.0, upper)


def command_compare(args: argparse.Namespace) -> None:
    baseline = load_json(Path(args.baseline).resolve())
    candidate = load_json(Path(args.candidate).resolve())
    baseline_manifest = baseline.get("manifest")
    candidate_manifest = candidate.get("manifest")
    mismatches = []
    if baseline_manifest and candidate_manifest:
        for field in ("maps", "rounds", "scheduled_games", "frame_limit", "timeout_limits"):
            if baseline_manifest.get(field) != candidate_manifest.get(field):
                mismatches.append(field)
        def opponents(manifest: dict[str, Any]) -> list[tuple[str, str]]:
            our_name = manifest.get("our_bot")
            return sorted(
                (str(bot.get("name")), str(bot.get("sha256")))
                for bot in manifest.get("bots", [])
                if bot.get("name") != our_name
            )
        if opponents(baseline_manifest) != opponents(candidate_manifest):
            mismatches.append("opponent binaries")
    if mismatches and not args.allow_mismatch:
        raise LadderError(
            "Runs are not an apples-to-apples comparison (" + ", ".join(mismatches) + "); "
            "use --allow-mismatch only for exploratory analysis"
        )
    a, b = baseline["summary"], candidate["summary"]
    delta, low, high = difference_interval(a["wins"], a["scored_games"], b["wins"], b["scored_games"])
    verdict = "likely improvement" if low > 0 else "likely regression" if high < 0 else "inconclusive"
    comparison: dict[str, Any] = {
        "generated_utc": utc_now(),
        "baseline": {"label": (baseline.get("manifest") or {}).get("label", "baseline"), **a},
        "candidate": {"label": (candidate.get("manifest") or {}).get("label", "candidate"), **b},
        "win_rate_delta": delta,
        "delta_95": [low, high],
        "verdict": verdict,
        "segments": {},
    }
    common_opponents = set(baseline.get("by_opponent", {})) & set(candidate.get("by_opponent", {}))
    for opponent in sorted(common_opponents):
        old, new = baseline["by_opponent"][opponent], candidate["by_opponent"][opponent]
        segment_delta, segment_low, segment_high = difference_interval(old["wins"], old["games"], new["wins"], new["games"])
        comparison["segments"][opponent] = {"delta": segment_delta, "delta_95": [segment_low, segment_high]}
    if args.output:
        write_json(Path(args.output).resolve(), comparison)
    print(
        f"{verdict}: candidate {percent(b['win_rate'])} vs baseline {percent(a['win_rate'])}; "
        f"delta {delta * 100:+.1f} points (95% CI {low * 100:+.1f} to {high * 100:+.1f})."
    )
    for opponent, item in comparison["segments"].items():
        print(f"  {opponent}: {item['delta'] * 100:+.1f} points")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    init = subparsers.add_parser("init", help="create and verify the ignored local ladder vault")
    init.add_argument("--config", default=str(DEFAULT_CONFIG))
    init.set_defaults(func=command_init)

    audit = subparsers.add_parser("audit", help="fail if any private ladder asset could be committed")
    audit.set_defaults(func=command_audit)

    add = subparsers.add_parser("add-bot", help="copy an opponent into the ignored local vault")
    add.add_argument("--config", default=str(DEFAULT_CONFIG))
    add.add_argument("--name", required=True)
    add.add_argument("--race", required=True, choices=("Protoss", "Terran", "Zerg", "Random"))
    add.add_argument("--type", dest="bot_type", required=True, choices=("dll", "proxy"))
    add.add_argument("--bwapi-version", default="BWAPI_440")
    add.add_argument("--source", required=True)
    add.add_argument("--source-url")
    add.add_argument("--version")
    add.add_argument("--license")
    add.set_defaults(func=command_add_bot)

    prepare = subparsers.add_parser("prepare", help="snapshot a reproducible Tournament Manager run")
    prepare.add_argument("--config", default=str(DEFAULT_CONFIG))
    prepare.add_argument("--label")
    prepare.set_defaults(func=command_prepare)

    report = subparsers.add_parser("report", help="analyze Tournament Manager raw or detailed results")
    report.add_argument("results", nargs="+")
    report.add_argument("--config", default=str(DEFAULT_CONFIG))
    report.add_argument("--manifest")
    report.add_argument("--our-bot")
    report.add_argument("--astra-log", action="append")
    report.add_argument("--output")
    report.set_defaults(func=command_report)

    compare = subparsers.add_parser("compare", help="compare two generated report.json files")
    compare.add_argument("baseline")
    compare.add_argument("candidate")
    compare.add_argument("--output")
    compare.add_argument("--allow-mismatch", action="store_true")
    compare.set_defaults(func=command_compare)
    return parser


def main() -> int:
    try:
        args = build_parser().parse_args()
        args.func(args)
        return 0
    except (LadderError, OSError, ValueError) as error:
        print(f"ladder: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
