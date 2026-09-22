"""Resume a pinned replay audit with more workers, without changing its checks.

The original identity and all training inputs remain immutable. Scheduling-only
provenance is recorded separately, so an already-running extractor can continue
consuming the same SQLite database and enforce its existing training gates.
"""
import argparse
from concurrent.futures import FIRST_COMPLETED, ThreadPoolExecutor, wait
from contextlib import closing
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import sqlite3
import time
from types import SimpleNamespace

from training.playback_check import check_one
from training.replay_assets import verify
from training.schema import sha256


def write_json(path, value):
    temp = path.with_suffix(path.suffix + ".tmp")
    temp.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")
    temp.replace(path)


def pinned_options(config_path):
    config = json.loads(Path(config_path).read_text())
    paths = {key: Path(config[key]).resolve() for key in
             ("node", "backend", "native", "decoder", "mpq", "assets", "output", "audit", "root")}
    identity_path = paths["output"] / "identity.json"
    identity = json.loads(identity_path.read_text())
    if identity["config"] != config:
        raise ValueError("Original validation config changed")
    if verify(paths["assets"]) != identity["terrain"]:
        raise ValueError("Pinned terrain changed")
    for name, digest in identity["hashes"].items():
        if sha256(name) != digest:
            raise ValueError(f"Pinned validation input changed: {name}")
    options = SimpleNamespace(**paths, interval=config.get("interval", 240), timeout=config.get("timeout", 300))
    return options, identity_path


def pending_tasks(db, tasks):
    existing = dict(db.execute("SELECT path,sha256 FROM results"))
    expected = dict(tasks)
    if any(path not in expected for path in existing):
        raise ValueError("Validation contains replays outside the frozen audit")
    changed = [path for path, digest in tasks if path in existing and existing[path] != digest]
    if changed:
        raise ValueError("Previously validated replay identity changed")
    return [(path, digest) for path, digest in tasks if path not in existing]


def execute_tasks(tasks, options, workers, accept, tick, checker=check_one):
    """Bound submissions; retry congestion-related timeouts after workers drain."""
    pending = iter(tasks)
    retries = []
    with ThreadPoolExecutor(max_workers=workers) as pool:
        futures = {}

        def submit():
            item = next(pending, None)
            if item is not None:
                path, digest = item
                futures[pool.submit(checker, options.root / path, options)] = (path, digest)

        for _ in range(workers):
            submit()
        while futures:
            done, _ = wait(futures, timeout=30, return_when=FIRST_COMPLETED)
            for future in done:
                path, digest = futures.pop(future)
                record = future.result()
                if "timed out" in record.get("error", "").lower():
                    retries.append((path, digest, record))
                else:
                    accept(path, digest, record)
                submit()
            tick()
    for path, digest, previous in retries:
        record = checker(options.root / path, options)
        record["parallel_timeout_retry"] = previous.get("error")
        accept(path, digest, record)
        tick()


def checked_record(record, expected):
    if record.get("sha256") != expected:
        record = {**record, "status": "quarantined", "error": "Replay changed since command audit"}
    if record.get("status") not in ("checkpoints_matched", "quarantined"):
        raise ValueError("Unexpected checker status")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=8)
    args = parser.parse_args()
    if not 1 <= args.workers <= 12:
        parser.error("workers must be 1..12")
    options, identity_path = pinned_options(args.config)
    # Stop the original validator before starting this scheduling replacement.
    # The extractor is a reader and must remain running.
    lock = options.output / "parallel-validator.lock"
    with lock.open("x") as stream:
        stream.write(str(os.getpid()))
    try:
        with closing(sqlite3.connect(options.audit.as_uri() + "?mode=ro", uri=True)) as source:
            tasks = source.execute("SELECT path,sha256 FROM replays ORDER BY path").fetchall()
        with closing(sqlite3.connect(options.output / "validation.sqlite", timeout=30)) as db:
            pending = pending_tasks(db, tasks)
            execution = {
                "started_at": datetime.now(timezone.utc).isoformat(), "pid": os.getpid(),
                "workers": args.workers, "original_workers": json.loads(identity_path.read_text())["config"].get("workers", 4),
                "scheduler": str(Path(__file__).resolve()), "scheduler_sha256": sha256(__file__),
                "validation_identity_sha256": sha256(identity_path), "audit_sha256": sha256(options.audit),
                "resumed_results": len(tasks) - len(pending), "pending": len(pending),
                "same_checker": "training.playback_check.check_one", "interval": options.interval,
                "timeout": options.timeout, "checks_changed": False,
                "timeout_policy": "retry once after parallel workers drain; same checks and timeout",
            }
            execution_path = options.output / f"parallel-execution-{os.getpid()}.json"
            if execution_path.exists():
                raise ValueError("Execution provenance already exists")
            write_json(execution_path, execution)
            started = time.monotonic()
            finished = 0

            def summary(complete=False):
                counts = dict(db.execute("SELECT status,count(*) FROM results GROUP BY status"))
                processed = sum(counts.values())
                elapsed = time.monotonic() - started
                value = {
                    "updated_at": datetime.now(timezone.utc).isoformat(), "total": len(tasks),
                    "processed": processed, "pending": len(tasks) - processed,
                    "matched": counts.get("checkpoints_matched", 0), "quarantined": counts.get("quarantined", 0),
                    "complete": complete and processed == len(tasks), "workers": args.workers,
                    "training_ready": False, "elapsed_this_run_seconds": round(elapsed, 1),
                    "resumed_results": execution["resumed_results"], "processed_this_run": finished,
                    "estimated_remaining_hours": round((len(tasks) - processed) * elapsed / finished / 3600, 2)
                        if finished >= args.workers * 2 else None,
                    "execution_manifest": str(execution_path),
                    "execution_manifest_sha256": sha256(execution_path),
                }
                write_json(options.output / "summary.json", value)
                return value

            def accept(path, expected, record):
                nonlocal finished
                record = checked_record(record, expected)
                db.execute("INSERT INTO results VALUES(?,?,?,?)",
                           (path, expected, record["status"], json.dumps(record)))
                db.commit()
                finished += 1
                print(json.dumps({"path": path, "status": record["status"],
                                  "seconds": record.get("elapsed_seconds"), "error": record.get("error"),
                                  "difference": record.get("first_difference")}), flush=True)

            summary()
            execute_tasks(pending, options, args.workers, accept, summary)
            # A completed result is published only after rechecking the same pins.
            pinned_options(args.config)
            if sha256(__file__) != execution["scheduler_sha256"] or sha256(options.audit) != execution["audit_sha256"]:
                raise ValueError("Scheduling code or frozen audit changed during run")
            print(json.dumps(summary(complete=True)), flush=True)
    finally:
        lock.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
