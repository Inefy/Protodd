"""Resumable, bounded full-corpus playback validation. No training flags are inferred."""
import argparse
from concurrent.futures import ThreadPoolExecutor, wait, FIRST_COMPLETED
from datetime import datetime, timezone
import json
from pathlib import Path
import sqlite3
import time
from types import SimpleNamespace

from training.playback_check import check_one, ROOT
from training.replay_assets import verify
from training.schema import sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    args = parser.parse_args()
    config = json.loads(args.config.read_text())
    workers = config.get("workers", 4)
    if type(workers) is not int or not 1 <= workers <= 4:
        raise ValueError("workers must be 1..4")
    paths = {key: Path(config[key]).resolve() for key in
             ("node", "backend", "native", "decoder", "mpq", "assets", "output", "audit", "root")}
    options = SimpleNamespace(**paths, interval=config.get("interval", 240), timeout=config.get("timeout", 300))
    output = paths["output"]
    output.mkdir(parents=True, exist_ok=True)
    identity = {"config": config, "terrain": verify(paths["assets"]), "hashes": {}}
    for path in [paths["node"], paths["native"], paths["decoder"],
                 ROOT / "training/playback_check.py", ROOT / "training/compare_replay.mjs",
                 ROOT / "training/validate_corpus.py", paths["backend"] / "dist/bwsim.js",
                 paths["backend"] / "bwsim_wasm.bwforge.wasm",
                 *[paths["mpq"] / n for n in ("Patch_rt.mpq", "BrooDat.mpq", "StarDat.mpq")]]:
        identity["hashes"][str(path)] = sha256(path)
    pin = output / "identity.json"
    if pin.exists():
        if json.loads(pin.read_text()) != identity:
            raise ValueError("Validation inputs changed; use a new run directory")
    else:
        pin.write_text(json.dumps(identity, indent=2) + "\n")
    source = sqlite3.connect(f"{paths['audit'].as_uri()}?mode=ro", uri=True)
    tasks = source.execute("SELECT path,sha256 FROM replays ORDER BY path").fetchall()
    source.close()
    db = sqlite3.connect(output / "validation.sqlite")
    db.execute("CREATE TABLE IF NOT EXISTS results (path TEXT PRIMARY KEY, sha256 TEXT NOT NULL, status TEXT NOT NULL, detail TEXT NOT NULL)")
    existing = dict(db.execute("SELECT path,sha256 FROM results"))
    pending = [(path, digest) for path, digest in tasks if existing.get(path) != digest]
    started = time.monotonic()
    finished_this_run = 0

    def summary():
        counts = dict(db.execute("SELECT status,count(*) FROM results GROUP BY status"))
        processed = sum(counts.values())
        elapsed = time.monotonic() - started
        result = {"updated_at": datetime.now(timezone.utc).isoformat(), "total": len(tasks),
                  "processed": processed, "pending": len(tasks) - processed,
                  "matched": counts.get("checkpoints_matched", 0),
                  "quarantined": counts.get("quarantined", 0), "complete": processed == len(tasks),
                  "workers": workers, "training_ready": False,
                  "elapsed_this_run_seconds": round(elapsed, 1),
                  "estimated_remaining_hours": round((len(tasks) - processed) * elapsed /
                                                      max(1, finished_this_run) / 3600, 2)}
        temp = output / "summary.tmp"
        temp.write_text(json.dumps(result, indent=2) + "\n")
        temp.replace(output / "summary.json")
        return result

    summary()
    iterator = iter(pending)
    with ThreadPoolExecutor(max_workers=workers) as executor:
        futures = {}
        def submit():
            item = next(iterator, None)
            if item:
                path, digest = item
                futures[executor.submit(check_one, paths["root"] / path, options)] = (path, digest)
        for _ in range(workers):
            submit()
        while futures:
            done, _ = wait(futures, timeout=30, return_when=FIRST_COMPLETED)
            for future in done:
                path, expected = futures.pop(future)
                record = future.result()
                if record.get("sha256") != expected:
                    record.update(status="quarantined", error="Replay changed since command audit")
                db.execute("INSERT OR REPLACE INTO results VALUES(?,?,?,?)",
                           (path, expected, record["status"], json.dumps(record)))
                db.commit()
                finished_this_run += 1
                print(json.dumps({"path": path, "status": record["status"],
                                  "seconds": record["elapsed_seconds"],
                                  "error": record.get("error"), "difference": record.get("first_difference")}), flush=True)
                submit()
            state = summary()
    print(json.dumps(state), flush=True)
    db.close()


if __name__ == "__main__":
    main()
