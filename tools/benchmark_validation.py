"""Compare worker counts using unchanged validation and exact reference results."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from copy import copy
import json
from pathlib import Path
import sqlite3
import time

from tools.validate_parallel import pinned_options, write_json
from training.playback_check import check_one
from training.schema import sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--reference", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--workers", nargs="+", type=int, default=[4, 8])
    args = parser.parse_args()
    if any(not 1 <= workers <= 12 for workers in args.workers):
        parser.error("workers must be 1..12")
    options, identity = pinned_options(args.config)
    records = json.loads(args.reference.read_text())["records"]
    # Distributed across the original three-matchup/nine-map sample.
    selected = [records[i] for i in (0, 1, 3, 5, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 25, 26)]
    with sqlite3.connect(options.output / "validation.sqlite") as db:
        failed = db.execute("SELECT detail FROM results WHERE status='quarantined' ORDER BY path").fetchall()
    for row in failed:
        record = json.loads(row[0])
        if record.get("mismatching_checkpoints", 0) > 0:
            selected.append(record)
            break
    for record in selected:
        if sha256(record["replay"]) != record["sha256"]:
            raise ValueError("Benchmark replay changed")
    args.output.mkdir(parents=True, exist_ok=False)
    report = {"validation_identity_sha256": sha256(identity), "replays": len(selected),
              "includes_known_divergence": len(selected) > 16, "runs": []}
    invariant = ("sha256", "status", "end_frame", "checkpoint_interval", "checkpoints",
                 "mismatching_checkpoints", "first_difference", "native_checkpoints_sha256",
                 "reference_checkpoints_sha256", "compared_unit_fields", "compared_player_fields",
                 "excluded_unit_state", "authoritative_game_validated", "training_ready")
    for workers in args.workers:
        run_options = copy(options)
        run_options.output = args.output / f"workers-{workers}"
        run_options.output.mkdir()
        started = time.monotonic()
        with ThreadPoolExecutor(max_workers=workers) as pool:
            actual = list(pool.map(lambda record: check_one(Path(record["replay"]), run_options), selected))
        seconds = time.monotonic() - started
        for expected, result in zip(selected, actual):
            for key in invariant:
                if result.get(key) != expected.get(key):
                    write_json(args.output / "failed-comparison.json", {"key": key, "expected": expected, "actual": result})
                    raise ValueError(f"Worker benchmark changed {key}: {expected['replay']}")
        run = {"workers": workers, "seconds": round(seconds, 3),
               "replays_per_hour": round(len(actual) / seconds * 3600, 1),
               "all_results_identical": True, "records": actual}
        report["runs"].append(run)
        write_json(args.output / "report.json", report)
        print(json.dumps({key: value for key, value in run.items() if key != "records"}), flush=True)
    report["passed"] = True
    write_json(args.output / "report.json", report)


if __name__ == "__main__":
    main()
