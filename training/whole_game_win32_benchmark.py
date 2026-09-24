"""Repeat a late-game Win32 six-slot inference fixture with frozen provenance."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path
import re
import statistics
import struct
import subprocess
import tempfile
from datetime import datetime, timezone

from .whole_game_multislot_promotion import require_win32_pe


SCHEMA = "protodd-whole-game-win32-forward-benchmark-v1"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def fixture_entities(path):
    with Path(path).open("rb") as stream:
        header = stream.read(20)
    if len(header) != 20 or header[:8] != b"PWGI1\0\0\0":
        raise ValueError("not a whole-game probe fixture")
    height, width, entities = struct.unpack("<III", header[8:])
    if not (1 <= height <= 512 and 1 <= width <= 512 and 100 <= entities <= 4096):
        raise ValueError("late-game fixture needs at least 100 legal entities")
    return entities


def active_slots(path):
    with Path(path).open("rb") as stream:
        if stream.read(8) != b"PWGO1\0\0\0":
            raise ValueError("compiled output magic mismatch")
        count_bytes = stream.read(4)
        if len(count_bytes) != 4:
            raise ValueError("truncated compiled output")
        count = struct.unpack("<I", count_bytes)[0]
        if count > 128:
            raise ValueError("compiled output has too many tensors")
        slots = None
        for _ in range(count):
            raw = stream.read(4)
            if len(raw) != 4:
                raise ValueError("truncated tensor name")
            length = struct.unpack("<I", raw)[0]
            if length > 128:
                raise ValueError("compiled output tensor name too long")
            name = stream.read(length).decode("ascii")
            raw = stream.read(4)
            if len(raw) != 4:
                raise ValueError("truncated tensor length")
            elements = struct.unpack("<I", raw)[0]
            if elements > 10_000_000:
                raise ValueError("compiled output tensor too large")
            if name == "slot_count":
                if elements != 1 or slots is not None:
                    raise ValueError("malformed slot count")
                value = stream.read(4)
                if len(value) != 4:
                    raise ValueError("truncated slot count")
                slots = struct.unpack("<f", value)[0]
            else:
                stream.seek(elements * 4, 1)
        if stream.read(1):
            raise ValueError("trailing compiled output bytes")
    if slots is None or not math.isfinite(slots) or slots != int(slots) or not 0 <= slots <= 6:
        raise ValueError("compiled output lacks a legal six-slot count")
    return int(slots)


def benchmark(package, executable, fixture, output, *, runs=20):
    package, executable, fixture, output = map(Path, (package, executable, fixture, output))
    if runs < 20 or output.exists():
        raise ValueError("benchmark needs at least 20 runs and a new output path")
    require_win32_pe(executable)
    weights = package / "weights.bin"
    manifest = json.loads((package / "manifest.json").read_text(encoding="utf8"))
    if (manifest.get("schema") != "protodd-whole-game-multislot-weights-v2" or
            manifest.get("maximum_slots") != 6 or
            manifest.get("weights_sha256") != digest(weights)):
        raise ValueError("benchmark package is not a verified six-slot export")
    entities = fixture_entities(fixture)
    samples = []
    expected_output = None
    slots = None
    with tempfile.TemporaryDirectory() as directory:
        result = Path(directory) / "result.bin"
        for _ in range(runs):
            process = subprocess.run(
                [str(executable.resolve()), str(weights.resolve()),
                 str(fixture.resolve()), str(result)],
                capture_output=True, text=True, check=True, timeout=120)
            match = re.search(r"\bforward_ms=([0-9.]+)\b", process.stdout)
            if not match:
                raise ValueError("Win32 probe did not report inference time")
            elapsed = float(match.group(1))
            if not math.isfinite(elapsed) or elapsed <= 0:
                raise ValueError("invalid inference time")
            current_slots = active_slots(result)
            current_output = digest(result)
            if expected_output is None:
                expected_output, slots = current_output, current_slots
            elif current_output != expected_output or current_slots != slots:
                raise ValueError("Win32 inference changed across identical inputs")
            samples.append(elapsed)
    ordered = sorted(samples)
    report = dict(schema=SCHEMA, architecture="Win32",
                  recorded_utc=datetime.now(timezone.utc).isoformat(),
                  probe_sha256=digest(executable), weights_sha256=digest(weights),
                  input_sha256=digest(fixture), output_sha256=expected_output,
                  entities=entities, active_slots=slots, samples_ms=samples,
                  median_ms=statistics.median(samples),
                  p95_ms=ordered[math.ceil(runs * 0.95) - 1], max_ms=ordered[-1],
                  full_callback_measured=False, tournament_ready=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "executable", "fixture", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--runs", type=int, default=20)
    args = parser.parse_args()
    print(json.dumps(benchmark(args.package, args.executable, args.fixture,
                               args.output, runs=args.runs), indent=2))


if __name__ == "__main__":
    main()
