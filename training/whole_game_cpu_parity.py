"""Compare a compiled CPU whole-game forward pass to PyTorch on legal replay views."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

import numpy as np
import torch

from .whole_game_export import read_package
from .whole_game_features import encode_observation, static_grid
from .whole_game_model import WholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def write_input(path, batch, memory):
    count = batch["type"].shape[1]
    height, width = batch["spatial"].shape[-2:]
    with Path(path).open("wb") as stream:
        stream.write(b"PWGI1\0\0\0")
        stream.write(struct.pack("<III", height, width, count))
        for name in ("type", "relation", "order"):
            stream.write(batch[name].numpy().astype("<i4").tobytes())
        stream.write(batch["entity_numeric"].numpy().astype("<f4").tobytes())
        stream.write(batch["spatial"].numpy().astype("<f4").tobytes())
        stream.write(batch["global"].numpy().astype("<f4").tobytes())
        stream.write(struct.pack("<I", int(memory is not None)))
        if memory is not None:
            stream.write(memory.numpy().astype("<f4").tobytes())


def read_output(path):
    with Path(path).open("rb") as stream:
        if stream.read(8) != b"PWGO1\0\0\0":
            raise ValueError("compiled inference output magic mismatch")
        count = struct.unpack("<I", stream.read(4))[0]
        result = {}
        for _ in range(count):
            name_length = struct.unpack("<I", stream.read(4))[0]
            name = stream.read(name_length).decode("ascii")
            length = struct.unpack("<I", stream.read(4))[0]
            result[name] = np.frombuffer(stream.read(length * 4), dtype="<f4").copy()
        if stream.read(1):
            raise ValueError("compiled inference output has trailing bytes")
        return result


def check(package, checkpoint, executable, release, output, max_rows=2,
          tolerance=2e-4, target_frames=None):
    package, checkpoint, executable, release, output = map(
        Path, (package, checkpoint, executable, release, output))
    manifest, state = read_package(package)
    source = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if manifest["checkpoint_sha256"] != hashlib.sha256(checkpoint.read_bytes()).hexdigest():
        raise ValueError("package belongs to a different teacher checkpoint")
    model = WholeGameModel(width=manifest["width"],
                           mixture_components=manifest["mixture_components"]).eval()
    model.load_state_dict(state, strict=True)
    if any(not torch.equal(state[name], value) for name, value in source["state_dict"].items()):
        raise ValueError("exported weights differ from PyTorch checkpoint")
    output.mkdir(parents=True, exist_ok=False)
    cases = []
    targets = sorted(set(target_frames or ()))
    target_index = 0
    with torch.no_grad():
        for record, directory, _ in selected_shards(release, "train"):
            terrain = static_grid(load_terrain(directory))
            memory = None
            for row, target in trajectory_shard(directory):
                if not target["update_memory"]:
                    continue
                if targets and row["frame"] < targets[target_index]:
                    continue
                batch, _, overflow = encode_observation(row, terrain)
                reference = model(batch, memory)
                fixture = output / f"input-{len(cases)}.bin"
                result_path = output / f"result-{len(cases)}.bin"
                write_input(fixture, batch, memory)
                process = subprocess.run([str(executable), str(package / "weights.bin"),
                                          str(fixture), str(result_path)],
                                         capture_output=True, text=True, timeout=120, check=True)
                actual = read_output(result_path)
                errors = {}
                for name, value in reference.items():
                    expected = value.numpy().reshape(-1)
                    observed = actual[name]
                    if expected.shape != observed.shape:
                        raise ValueError(f"CPU output shape differs: {name}")
                    errors[name] = float(np.max(np.abs(expected - observed)))
                cases.append(dict(game_id=record["game_id"], frame=row["frame"],
                                  entities=batch["type"].shape[1], overflow=overflow,
                                  errors=errors, compiled_stdout=process.stdout.strip()))
                memory = reference["memory"]
                if targets:
                    target_index += 1
                    if target_index >= len(targets):
                        break
                if len(cases) >= max_rows:
                    break
            if len(cases) >= max_rows or (targets and target_index >= len(targets)):
                break
    if not cases:
        raise ValueError("no legal cadence observations selected")
    maximum = max(value for case in cases for value in case["errors"].values())
    report = dict(schema="protodd-whole-game-cpu-parity-v1", cases=cases,
                  max_abs_error=maximum, tolerance=tolerance,
                  passed=maximum <= tolerance, tournament_ready=False)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise ValueError(f"compiled CPU output mismatch: max error {maximum}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "checkpoint", "executable", "release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--max-rows", type=int, default=2)
    parser.add_argument("--frames", type=int, nargs="+",
                        help="Sample first legal cadence at or after each requested frame")
    args = parser.parse_args()
    report = check(args.package, args.checkpoint, args.executable, args.release,
                   args.output, args.max_rows, target_frames=args.frames)
    print(json.dumps(dict(passed=report["passed"], max_abs_error=report["max_abs_error"],
                          cases=len(report["cases"])), indent=2))


if __name__ == "__main__":
    main()
