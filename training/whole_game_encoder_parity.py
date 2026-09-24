"""Verify the compiled legal-observation encoder against Python replay features."""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct
import subprocess

import numpy as np

from .whole_game_features import encode_observation, static_grid
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


def fixture(path, row, terrain):
    height, width = terrain.shape
    with Path(path).open("wb") as stream:
        stream.write(b"PWGS1\0\0\0")
        stream.write(struct.pack("<IIiiiii", width, height, row["frame"], row["minerals"],
                                 row["gas"], row["supply_used"], row["supply_total"]))
        for name in ("technology_completed", "technology_in_progress",
                     "upgrade_levels", "upgrade_in_progress"):
            stream.write(np.asarray(row[name], dtype="<i4").tobytes())
        stream.write(row["vision"].encode("ascii"))
        stream.write(terrain.numpy().astype("<f4").tobytes())
        stream.write(struct.pack("<I", len(row["entities"])))
        for entity in row["entities"]:
            own = entity["own_state"] or {}
            position = own.get("order_position", [-1, -1])
            queue, cargo = own.get("queue", []), own.get("cargo", [])
            values = (entity["id"], entity["type"], entity["relation"],
                      *entity["position"], entity["hp"], entity["shields"],
                      entity["first_seen"], entity["last_seen"], int(entity["visible"]),
                      int(entity["completed"]), own.get("energy", 0),
                      own.get("ground_cooldown", 0), own.get("air_cooldown", 0),
                      own.get("order", 0), *position, int(own.get("loaded", 0)))
            stream.write(struct.pack("<" + "i" * len(values) + "II", *values, len(queue), len(cargo)))
            stream.write(np.asarray(queue, dtype="<i4").tobytes())
            stream.write(np.asarray(cargo, dtype="<i4").tobytes())


def decode(path):
    data = Path(path).read_bytes()
    if data[:8] != b"PWGI1\0\0\0":
        raise ValueError("compiled encoder output magic mismatch")
    height, width, count = struct.unpack_from("<III", data, 8)
    position = 20
    def take(dtype, length):
        nonlocal position
        result = np.frombuffer(data, dtype=dtype, count=length, offset=position).copy()
        position += result.nbytes
        return result
    result = {name: take("<i4", count) for name in ("type", "relation", "order")}
    result["entity_numeric"] = take("<f4", count * 16).reshape(count, 16)
    result["spatial"] = take("<f4", 3 * height * width).reshape(3, height, width)
    result["global"] = take("<f4", 216)
    has_memory, overflow = struct.unpack_from("<II", data, position)
    position += 8
    result["ids"] = take("<i4", count)
    if has_memory or position != len(data):
        raise ValueError("compiled encoder output has unexpected trailing data")
    return result, overflow


def check(release, executable, output, frames=(0, 5000, 15000, 30000)):
    release, executable, output = Path(release), Path(executable), Path(output)
    output.mkdir(parents=True, exist_ok=False)
    targets = sorted(set(frames))
    target_index = 0
    cases = []
    for record, directory, _ in selected_shards(release, "train"):
        terrain = static_grid(load_terrain(directory))
        for row, supervision in trajectory_shard(directory):
            if not supervision["update_memory"] or row["frame"] < targets[target_index]:
                continue
            expected, ids, overflow = encode_observation(row, terrain)
            source = output / f"snapshot-{len(cases)}.bin"
            target = output / f"encoded-{len(cases)}.bin"
            fixture(source, row, terrain)
            subprocess.run([str(executable), str(source), str(target)],
                           capture_output=True, text=True, timeout=30, check=True)
            actual, actual_overflow = decode(target)
            errors = {}
            for name in ("type", "relation", "order", "entity_numeric", "spatial", "global"):
                before = expected[name].numpy().reshape(actual[name].shape)
                errors[name] = float(np.max(np.abs(before - actual[name])))
            ordered_ids = [entity for entity, _ in sorted(ids.items(), key=lambda pair: pair[1])]
            if ordered_ids != actual["ids"].tolist() or overflow != actual_overflow:
                raise ValueError("compiled entity selection or overflow differs")
            cases.append(dict(game_id=record["game_id"], frame=row["frame"],
                              entities=len(ids), overflow=overflow, errors=errors))
            target_index += 1
            if target_index >= len(targets):
                break
        if target_index >= len(targets):
            break
    if not cases:
        raise ValueError("no encoder parity cases")
    maximum = max(value for case in cases for value in case["errors"].values())
    report = dict(schema="protodd-whole-game-encoder-parity-v1", cases=cases,
                  max_abs_error=maximum, passed=maximum <= 2e-6)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise ValueError(f"compiled encoder mismatch: {maximum}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("executable", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = check(args.release, args.executable, args.output)
    print(json.dumps(dict(passed=report["passed"], max_abs_error=report["max_abs_error"],
                          cases=len(report["cases"])), indent=2))


if __name__ == "__main__":
    main()
