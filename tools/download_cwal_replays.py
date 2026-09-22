#!/usr/bin/env python3
"""Download a large, resumable cwal.gg replay dataset.

The cwal replay API is cursor-paginated and exposes the replay bytes by match
ID.  This tool keeps the API catalog separate from the downloaded files so an
interrupted run can resume without starting over.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from pathlib import Path
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen


API_ROOT = "https://api.aws.cwal.gg"
MATCHUPS = ("ZvZ", "ZvT", "ZvP", "TvT", "TvP", "PvP")
DURATION_BUCKETS = (
    "110-300",
    "300-600",
    "600-1200",
    "1200-2400",
    "2400-3600",
    "3600+",
)
# cwal caps the expanded partition count at 32.  Asymmetric matchups expand
# this six-bucket query to 60 partitions, so query the two halves separately.
DURATION_GROUPS = (DURATION_BUCKETS[:3], DURATION_BUCKETS[3:])


def mmr_ranges(mmr_lo: int, mmr_hi: int) -> tuple[tuple[int, int], ...]:
    """Split wide MMR requests so asymmetric filters stay under cwal's cap."""
    ranges: list[tuple[int, int]] = []
    current = mmr_lo
    while current < mmr_hi:
        upper = min(current + 500, mmr_hi)
        ranges.append((current, upper))
        current = upper
    return tuple(ranges)


def request_json(url: str, retries: int = 5) -> dict[str, Any]:
    last_error: Exception | None = None
    for attempt in range(retries):
        try:
            request = Request(url, headers={"User-Agent": "protodd-cwal-dataset/1.0"})
            with urlopen(request, timeout=60) as response:
                return json.loads(response.read().decode("utf-8"))
        except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError) as error:
            last_error = error
            if attempt + 1 < retries:
                retry_after = None
                if isinstance(error, HTTPError):
                    try:
                        retry_after = float(error.headers.get("Retry-After", ""))
                    except ValueError:
                        retry_after = None
                time.sleep(max(retry_after or 0, min(2 ** (attempt + 1), 30)))
    raise RuntimeError(f"request failed after {retries} attempts: {url}: {last_error}")


def download_file(match_id: str, destination: Path, retries: int = 5) -> tuple[str, int, str | None]:
    """Download one replay atomically; return (match_id, byte_count, error)."""
    if destination.exists() and destination.stat().st_size > 0:
        return match_id, destination.stat().st_size, None

    partial = destination.with_suffix(destination.suffix + ".part")
    last_error: Exception | None = None
    for attempt in range(retries):
        try:
            request = Request(
                f"{API_ROOT}/api/replay/{match_id}/file",
                headers={"User-Agent": "protodd-cwal-dataset/1.0"},
            )
            with urlopen(request, timeout=180) as response, partial.open("wb") as output:
                total = 0
                while True:
                    block = response.read(1024 * 1024)
                    if not block:
                        break
                    output.write(block)
                    total += len(block)
                output.flush()
                os.fsync(output.fileno())
            if total == 0:
                raise RuntimeError("empty replay response")
            partial.replace(destination)
            return match_id, total, None
        except (HTTPError, URLError, TimeoutError, OSError, RuntimeError) as error:
            last_error = error
            try:
                partial.unlink()
            except FileNotFoundError:
                pass
            if attempt + 1 < retries:
                retry_after = None
                if isinstance(error, HTTPError):
                    try:
                        retry_after = float(error.headers.get("Retry-After", ""))
                    except ValueError:
                        retry_after = None
                time.sleep(max(retry_after or 0, min(2 ** (attempt + 1), 30)))
    return match_id, 0, str(last_error)


def append_jsonl(path: Path, value: dict[str, Any], lock: threading.Lock) -> None:
    line = json.dumps(value, ensure_ascii=False, sort_keys=True) + "\n"
    with lock:
        with path.open("a", encoding="utf-8") as output:
            output.write(line)


def load_catalog(path: Path) -> dict[str, dict[str, Any]]:
    records: dict[str, dict[str, Any]] = {}
    if not path.exists():
        return records
    with path.open(encoding="utf-8") as source:
        for line in source:
            if line.strip():
                record = json.loads(line)
                records[str(record["matchId"])] = record
    return records


def load_downloaded(path: Path) -> set[str]:
    downloaded: set[str] = set()
    if not path.exists():
        return downloaded
    with path.open(encoding="utf-8") as source:
        for line in source:
            if line.strip():
                record = json.loads(line)
                if record.get("status") == "downloaded":
                    downloaded.add(str(record["matchId"]))
    return downloaded


def write_records(path: Path, records: dict[str, dict[str, Any]]) -> None:
    with path.open("w", encoding="utf-8") as output:
        for record in records.values():
            output.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n")


def fetch_catalog(
    matchup: str,
    catalog_path: Path,
    target: int,
    mmr_lo: int,
    mmr_hi: int,
    status: callable,
) -> dict[str, dict[str, Any]]:
    records = load_catalog(catalog_path)
    for range_lo, range_hi in mmr_ranges(mmr_lo, mmr_hi):
        for duration_group in DURATION_GROUPS:
            cursor: str | None = None
            while len(records) < target:
                query: dict[str, str] = {
                    "matchup": matchup,
                    "mmrLo": str(range_lo),
                    "mmrHi": str(range_hi),
                    "durations": ",".join(duration_group),
                    "limit": "25",
                }
                if cursor:
                    query["cursor"] = cursor
                payload = request_json(f"{API_ROOT}/api/vault?{urlencode(query)}")
                page = payload.get("replays", [])
                before = len(records)
                for record in page:
                    records[str(record["matchId"])] = record
                status(f"{matchup}: catalog {len(records):,}/{target:,}")
                cursor = payload.get("nextCursor")
                if len(records) == before and not cursor:
                    break
                if not cursor:
                    break
                write_records(catalog_path, records)
            if len(records) >= target:
                break
        if len(records) >= target:
            break
    write_records(catalog_path, records)
    return records


def prepare_queue(
    selected: tuple[str, ...],
    root: Path,
    target: int,
    mmr_lo: int,
    mmr_hi: int,
    status,
) -> dict[str, dict[str, Any]]:
    """Discover all eligible metadata, then write a stable global download queue."""
    all_records: dict[str, dict[str, Any]] = {}
    catalog_path = root / "replay-catalog.jsonl"
    queue_path = root / "download-queue.jsonl"
    for matchup in selected:
        matchup_root = root / matchup
        matchup_root.mkdir(parents=True, exist_ok=True)
        records = fetch_catalog(
            matchup,
            matchup_root / "catalog.jsonl",
            target,
            mmr_lo,
            mmr_hi,
            status,
        )
        all_records.update(records)
        write_records(catalog_path, all_records)
        queue = [
            record
            for record in all_records.values()
            if not (root / str(record["matchup"]) / f"{record['matchId']}.rep").exists()
        ]
        write_records(queue_path, {str(record["matchId"]): record for record in queue})
        status(f"queue: {len(all_records):,} eligible, {len(queue):,} still to download")
    return all_records


def load_queue(path: Path) -> dict[str, dict[str, dict[str, Any]]]:
    records: dict[str, dict[str, dict[str, Any]]] = {matchup: {} for matchup in MATCHUPS}
    with path.open(encoding="utf-8") as source:
        for line in source:
            if line.strip():
                record = json.loads(line)
                matchup = str(record["matchup"])
                records.setdefault(matchup, {})[str(record["matchId"])] = record
    return records


def download_matchup(
    matchup: str,
    root: Path,
    target: int,
    workers: int,
    status,
    mmr_lo: int,
    mmr_hi: int,
    records: dict[str, dict[str, Any]] | None = None,
) -> tuple[int, int, int]:
    matchup_root = root / matchup
    matchup_root.mkdir(parents=True, exist_ok=True)
    catalog_path = matchup_root / "catalog.jsonl"
    manifest_path = matchup_root / "manifest.jsonl"
    if records is None:
        records = fetch_catalog(matchup, catalog_path, target, mmr_lo, mmr_hi, status)
    downloaded = load_downloaded(manifest_path)
    candidates = list(records.values())[:target]
    pending = [
        record
        for record in candidates
        if str(record["matchId"]) not in downloaded
        and not (matchup_root / f"{record['matchId']}.rep").exists()
    ]
    for record in candidates:
        path = matchup_root / f"{record['matchId']}.rep"
        if path.exists() and path.stat().st_size > 0:
            downloaded.add(str(record["matchId"]))

    lock = threading.Lock()
    success = len(downloaded)
    failures = 0
    status(f"{matchup}: downloading {len(pending):,} replays with {workers} workers")
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {
            pool.submit(download_file, str(record["matchId"]), matchup_root / f"{record['matchId']}.rep"): record
            for record in pending
        }
        for index, future in enumerate(as_completed(futures), 1):
            record = futures[future]
            match_id, size, error = future.result()
            if error is None:
                success += 1
                append_jsonl(
                    manifest_path,
                    {
                        "matchId": match_id,
                        "status": "downloaded",
                        "bytes": size,
                        "matchup": matchup,
                        "timestamp": record.get("timestamp"),
                    },
                    lock,
                )
            else:
                failures += 1
                append_jsonl(
                    manifest_path,
                    {"matchId": match_id, "status": "error", "error": error, "matchup": matchup},
                    lock,
                )
            if index % 25 == 0 or index == len(pending):
                status(f"{matchup}: {success:,}/{len(candidates):,} downloaded, {failures:,} errors")
    return len(candidates), success, failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path("artifacts/cwal-dataset"))
    parser.add_argument("--target-per-matchup", type=int, default=10_000)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--mmr-lo", type=int, default=2400)
    parser.add_argument("--mmr-hi", type=int, default=2900)
    parser.add_argument("--matchup", action="append", choices=MATCHUPS, dest="matchups")
    parser.add_argument("--prepare-queue", action="store_true")
    parser.add_argument("--prepare-and-download", action="store_true")
    parser.add_argument("--queue", type=Path)
    args = parser.parse_args()
    if args.target_per_matchup <= 0 or args.workers <= 0:
        parser.error("target and workers must be positive")
    if args.mmr_lo < 0 or args.mmr_hi <= args.mmr_lo:
        parser.error("mmr-hi must be greater than mmr-lo")

    root = args.root.resolve()
    root.mkdir(parents=True, exist_ok=True)
    selected = tuple(args.matchups or MATCHUPS)
    if (args.prepare_queue or args.prepare_and_download) and args.queue:
        parser.error("queue input cannot be combined with queue preparation")
    if args.prepare_queue and args.prepare_and_download:
        parser.error("--prepare-queue and --prepare-and-download cannot be combined")
    queue_records = None
    if args.prepare_queue or args.prepare_and_download:
        records = prepare_queue(
            selected,
            root,
            args.target_per_matchup,
            args.mmr_lo,
            args.mmr_hi,
            print,
        )
        queue_path = root / "download-queue.jsonl"
        if args.prepare_and_download:
            queue_records = load_queue(queue_path)
        else:
            queue_count = sum(1 for _ in queue_path.open(encoding="utf-8"))
            (root / "dataset-summary.json").write_text(
                json.dumps(
                    {
                        "source": "https://cwal.gg/replays",
                        "api": API_ROOT,
                        "filters": {
                            "mmrLo": args.mmr_lo,
                            "mmrHi": args.mmr_hi,
                            "durations": list(DURATION_BUCKETS),
                        },
                        "targetPerMatchup": args.target_per_matchup,
                        "eligible": len(records),
                        "queue": queue_count,
                        "queuePath": str(queue_path),
                    },
                    indent=2,
                )
                + "\n",
                encoding="utf-8",
            )
            print(f"wrote {queue_path}")
            return 0
    elif args.queue:
        queue_records = load_queue(args.queue.resolve())
    summary: dict[str, dict[str, int]] = {}
    for matchup in selected:
        try:
            records = queue_records.get(matchup, {}) if queue_records is not None else None
            summary[matchup] = dict(
                zip(
                    ("catalog", "downloaded", "errors"),
                    download_matchup(
                        matchup,
                        root,
                        args.target_per_matchup,
                        args.workers,
                        print,
                        args.mmr_lo,
                        args.mmr_hi,
                        records,
                    ),
                )
            )
        except KeyboardInterrupt:
            print("interrupted; files and catalogs are resumable", file=sys.stderr)
            return 130
        except Exception as error:
            print(f"{matchup}: fatal error: {error}", file=sys.stderr)
            summary[matchup] = {"catalog": 0, "downloaded": 0, "errors": 1}

    # Recompute the final summary from disk.  This remains accurate when the
    # downloader consumed a prebuilt queue or recovered from an interrupted run.
    for matchup in selected:
        matchup_root = root / matchup
        records = load_catalog(matchup_root / "catalog.jsonl")
        files = [
            path
            for path in matchup_root.glob("*.rep")
            if path.is_file() and path.stat().st_size > 0
        ]
        missing = [
            record
            for record in records.values()
            if not (matchup_root / f"{record['matchId']}.rep").exists()
        ]
        summary[matchup] = {
            "catalog": len(records),
            "downloaded": len(files),
            "errors": len(missing),
        }

    (root / "dataset-summary.json").write_text(
        json.dumps(
            {
                "source": "https://cwal.gg/replays",
                "api": API_ROOT,
                "filters": {
                        "mmrLo": args.mmr_lo,
                        "mmrHi": args.mmr_hi,
                    "durations": list(DURATION_BUCKETS),
                },
                "targetPerMatchup": args.target_per_matchup,
                "summary": summary,
            },
            indent=2,
        )
        + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
