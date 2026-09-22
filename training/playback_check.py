"""Compare native replay playback to bwsim; quarantine errors, never certify training."""
from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import time
from pathlib import Path

from training.replay_assets import digest, verify

ROOT = Path(__file__).resolve().parents[1]


def run_command(command: list[str], timeout: int, output=None) -> str:
    result = subprocess.run(command, stdout=output or subprocess.PIPE, stderr=subprocess.PIPE,
                            timeout=timeout, check=False)
    if result.returncode:
        # The comparator returns structured mismatch details with exit code 1.
        details = (result.stdout or b"").decode("utf-8", errors="replace").strip()
        if details.startswith('{"status":"quarantined"'):
            return details
        raise ValueError(f"{Path(command[0]).name} failed ({result.returncode}): "
                         + result.stderr.decode("utf-8", errors="replace")[-2000:])
    return (result.stdout or b"").decode("utf-8")


def check_one(replay: Path, args) -> dict:
    start = time.monotonic()
    record = {"replay": str(replay.resolve()), "status": "quarantined", "training_ready": False,
              "authoritative_game_validated": False}
    try:
        with tempfile.TemporaryDirectory(prefix="replay-check-", dir=args.output) as directory:
            temp = Path(directory)
            # Hash and run the same immutable copy even if a downloader is active.
            data = replay.read_bytes()
            record["sha256"] = digest(data)
            copied = temp / "input.rep"
            copied.write_bytes(data)
            decoded = temp / "replay.raw"
            record["conversion"] = json.loads(run_command(
                [str(args.decoder), str(copied), str(decoded)], args.timeout))
            states = temp / "checkpoints.jsonl"
            with states.open("wb") as stream:
                run_command([str(args.native), str(args.mpq), str(args.assets), str(decoded),
                             str(args.interval)], args.timeout, output=stream)
            comparison = json.loads(run_command([str(args.node), str(ROOT / "training/compare_replay.mjs"),
                str(args.backend), str(copied), str(states), str(args.interval)], args.timeout))
            if comparison.get("status") not in ("checkpoints_matched", "quarantined"):
                raise ValueError("Invalid comparison result")
            record.update(comparison)
            record["training_ready"] = False
            record["authoritative_game_validated"] = False
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        record["error"] = str(error)
    record["elapsed_seconds"] = round(time.monotonic() - start, 3)
    return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("node", "backend", "native", "decoder", "mpq", "assets", "output"):
        parser.add_argument(f"--{name}", type=Path, required=True)
    parser.add_argument("--interval", type=int, default=240)
    parser.add_argument("--timeout", type=int, default=180)
    parser.add_argument("--replay-list", type=Path, help="JSON array of local replay paths")
    parser.add_argument("replays", type=Path, nargs="*")
    args = parser.parse_args()
    if not 1 <= args.interval <= 2400 or args.timeout < 1:
        parser.error("Invalid checkpoint interval/timeout")
    if args.replay_list:
        paths = json.loads(args.replay_list.read_text(encoding="utf-8"))
        if not isinstance(paths, list) or any(not isinstance(p, str) for p in paths):
            parser.error("Replay list must contain paths")
        args.replays.extend(Path(p) for p in paths)
    if not args.replays:
        parser.error("Provide replay paths or --replay-list")
    for name in ("node", "backend", "native", "decoder", "mpq", "assets", "output"):
        setattr(args, name, getattr(args, name).resolve())
    terrain = verify(args.assets)
    args.output.mkdir(parents=True, exist_ok=False)
    config = {"version": 1, "interval": args.interval, "terrain": terrain,
              "backend_provenance": json.loads((args.backend / "provenance.json").read_text()),
              "hashes": {}, "training_ready": False}
    for path in (args.native, args.decoder, ROOT / "training/compare_replay.mjs",
                 Path(__file__), ROOT / "tools/replay_native/main.cpp", args.backend / "dist/bwsim.js"):
        config["hashes"][str(path)] = digest(path.read_bytes())
    # Record the base archive identities as well as the terrain replacement.
    config["base_mpqs"] = {name: digest((args.mpq / name).read_bytes())
                           for name in ("Patch_rt.mpq", "BrooDat.mpq", "StarDat.mpq")}
    (args.output / "config.json").write_text(json.dumps(config, indent=2) + "\n", encoding="utf-8")
    records = []
    with (args.output / "results.jsonl").open("x", encoding="utf-8") as stream:
        for replay in args.replays:
            record = check_one(replay, args)
            records.append(record)
            stream.write(json.dumps(record, ensure_ascii=True) + "\n")
            stream.flush()
            print(f"{len(records)}/{len(args.replays)} {record['status']}: {replay.name} "
                  f"({record['elapsed_seconds']:.1f}s)", flush=True)
    summary = {"complete": True, "replays": len(records),
        "matched": sum(r["status"] == "checkpoints_matched" for r in records),
        "quarantined": sum(r["status"] != "checkpoints_matched" for r in records),
        "checkpoints": sum(r.get("checkpoints", 0) for r in records),
        "training_ready": False, "authoritative_game_validated": False}
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary), flush=True)
    raise SystemExit(1 if summary["quarantined"] else 0)


if __name__ == "__main__":
    main()
