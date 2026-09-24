"""Check a compiled six-slot package against PyTorch on legal replay views."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess

import numpy as np
import torch

from .whole_game_cpu_parity import read_output, write_input
from .whole_game_export import read_package
from .whole_game_features import encode_observation, static_grid
from .whole_game_multislot_export import SCHEMA
from .whole_game_multislot_model import MultiSlotWholeGameModel
from .whole_game_shards import load_terrain, selected_shards, trajectory_shard


COMPARE_HEADS = ("stop", "domain", "queued", "delay", "position", "order",
                 "unit_type", "technology", "upgrade", "queue_slot")


def require_win32_pe(executable):
    with Path(executable).open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise ValueError("compiled parity probe is not a Windows executable")
        stream.seek(0x3c)
        offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0" or struct.unpack("<H", stream.read(2))[0] != 0x14c:
            raise ValueError("compiled parity probe must be Win32 x86")


def check(package, checkpoint, executable, release, output, *, max_rows=3,
          tolerance=3e-4, min_frame=0):
    package, checkpoint, executable, release, output = map(
        Path, (package, checkpoint, executable, release, output))
    if max_rows < 1 or min_frame < 0 or output.exists():
        raise ValueError("invalid parity output or row limit")
    require_win32_pe(executable)
    manifest, state = read_package(package)
    if manifest["schema"] != SCHEMA or manifest["maximum_slots"] != 6:
        raise ValueError("compiled six-slot package required")
    if manifest["checkpoint_sha256"] != hashlib.sha256(checkpoint.read_bytes()).hexdigest():
        raise ValueError("package belongs to another checkpoint")
    saved = torch.load(checkpoint, map_location="cpu", weights_only=True)
    if set(state) != set(saved["state_dict"]) or any(
            not torch.equal(value, saved["state_dict"][name])
            for name, value in state.items()):
        raise ValueError("exported parameters differ from checkpoint")
    model = MultiSlotWholeGameModel(width=manifest["width"],
                                    mixture_components=manifest["mixture_components"],
                                    maximum_slots=6).eval()
    model.load_state_dict(state, strict=True)
    torch.set_num_threads(2)
    output.mkdir(parents=True)
    cases = []
    sampled_matchups = set()
    with torch.inference_mode():
        for record, directory, _ in selected_shards(release, "validation"):
            if record["matchup"] in sampled_matchups:
                continue
            terrain = static_grid(load_terrain(directory))
            memory = None
            for row, target in trajectory_shard(directory):
                if not target["update_memory"]:
                    continue
                batch, ids, overflow = encode_observation(row, terrain)
                reference = model.forward_slots(batch, memory)
                if row["frame"] < min_frame:
                    memory = reference["backbone"]["memory"]
                    continue
                index = len(cases)
                fixture = output / f"input-{index}.bin"
                result = output / f"result-{index}.bin"
                write_input(fixture, batch, memory)
                process = subprocess.run(
                    [str(executable), str(package / "weights.bin"),
                     str(fixture), str(result)],
                    capture_output=True, text=True, check=True, timeout=120)
                actual = read_output(result)
                active = next((slot for slot, prediction in
                               enumerate(reference["slots"])
                               if int(prediction["chosen_stop"][0]) == 1), 6)
                if int(actual["slot_count"][0]) != active:
                    raise ValueError("compiled STOP decision differs from PyTorch")
                errors = dict(memory=float(np.max(np.abs(
                    actual["memory"] - reference["backbone"]["memory"].numpy().reshape(-1)))))
                # Check every active autoregressive slot, including later slots
                # whose hidden state depends on all preceding sampled tokens.
                for slot_index in range(active):
                    prediction = reference["slots"][slot_index]
                    prefix = f"slot{slot_index}."
                    for name in COMPARE_HEADS:
                        expected = prediction[name].numpy().reshape(-1)
                        observed = actual[prefix + name]
                        if expected.shape != observed.shape:
                            raise ValueError(f"compiled slot shape differs: {prefix}{name}")
                        errors[prefix + name] = float(np.max(np.abs(expected - observed)))
                    for name in ("actor", "kind", "target_mode"):
                        observed = int(actual[prefix + name].argmax())
                        expected = int(prediction["chosen_" + name][0])
                        if observed != expected:
                            raise ValueError(f"compiled slot choice differs: {prefix}{name}")
                    if int(prediction["chosen_target_mode"][0]) == 1:
                        observed = int(actual[prefix + "target"].argmax())
                        expected = int(prediction["chosen_target_entity"][0])
                        if observed != expected:
                            raise ValueError(f"compiled target differs: {prefix}target")
                match = re.search(r"forward_ms=([0-9.]+)", process.stdout)
                cases.append(dict(game_id=record["game_id"], matchup=record["matchup"],
                                  frame=row["frame"],
                                  entities=len(ids), overflow=overflow, active_slots=active,
                                  compiled_ms=float(match.group(1)) if match else None,
                                  max_abs_error=max(errors.values()), errors=errors))
                sampled_matchups.add(record["matchup"])
                memory = reference["backbone"]["memory"]
                break
            if len(cases) >= max_rows:
                break
    if not cases:
        raise ValueError("no validation cadence observations found")
    if max_rows >= 3 and len(sampled_matchups) != 3:
        raise ValueError("parity must cover all three Protoss matchups")
    maximum = max(case["max_abs_error"] for case in cases)
    report = dict(schema="protodd-whole-game-multislot-cpu-parity-v1",
                  checkpoint_sha256=manifest["checkpoint_sha256"],
                  package_sha256=manifest["weights_sha256"], cases=cases,
                  executable_sha256=hashlib.sha256(executable.read_bytes()).hexdigest(),
                  architecture="Win32-x86",
                  min_frame=min_frame,
                  tolerance=tolerance, max_abs_error=maximum,
                  passed=maximum <= tolerance, tournament_ready=False)
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    if not report["passed"]:
        raise ValueError(f"compiled multi-slot output mismatch: {maximum}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("package", "checkpoint", "executable", "release", "output"):
        parser.add_argument(name, type=Path)
    parser.add_argument("--max-rows", type=int, default=3)
    parser.add_argument("--min-frame", type=int, default=0)
    args = parser.parse_args()
    report = check(args.package, args.checkpoint, args.executable,
                   args.release, args.output, max_rows=args.max_rows,
                   min_frame=args.min_frame)
    print(json.dumps(dict(passed=report["passed"],
                          max_abs_error=report["max_abs_error"],
                          cases=len(report["cases"])), indent=2))


if __name__ == "__main__":
    main()
