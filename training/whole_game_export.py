"""Deterministic float32 weight package for a compiled whole-game CPU runtime.

The package is an interchange artifact, not a playable bot. Deployment still
requires a numerically checked C++ evaluator, legal action arbiter, latency
tests and measured match strength.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct

import torch

from .whole_game_model import WholeGameModel


MAGIC = b"PWGM1\0\0\0"
SCHEMA = "protodd-whole-game-weights-v1"


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def package(checkpoint_path, output):
    checkpoint_path, output = Path(checkpoint_path), Path(output)
    checkpoint = torch.load(checkpoint_path, map_location="cpu", weights_only=True)
    if checkpoint.get("schema") != "protodd-whole-game-fit-v1":
        raise ValueError("unsupported teacher checkpoint")
    state = checkpoint["state_dict"]
    width = int(state["memory.weight_hh"].shape[1])
    components = int(state["position.weight"].shape[0] // 5)
    model = WholeGameModel(width=width, mixture_components=components)
    model.load_state_dict(state, strict=True)
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    rows = []
    binary = output / "weights.bin"
    with binary.open("wb") as stream:
        stream.write(MAGIC)
        stream.write(struct.pack("<III", 1, width, components))
        ordered = sorted(state.items())
        stream.write(struct.pack("<I", len(ordered)))
        for name, tensor in ordered:
            data = tensor.detach().cpu().contiguous().float().numpy().astype("<f4", copy=False)
            encoded = name.encode("ascii")
            if len(encoded) > 65535 or data.ndim > 4:
                raise ValueError("unsupported parameter name or rank")
            stream.write(struct.pack("<HB", len(encoded), data.ndim))
            stream.write(encoded)
            for dimension in data.shape:
                stream.write(struct.pack("<I", dimension))
            stream.write(data.tobytes(order="C"))
            rows.append(dict(name=name, shape=list(data.shape), elements=data.size))
    manifest = dict(schema=SCHEMA, checkpoint_sha256=digest(checkpoint_path),
                    weights_sha256=digest(binary), source_identity_sha256=checkpoint["source_identity_sha256"],
                    width=width, mixture_components=components,
                    parameters=sum(row["elements"] for row in rows), tensors=rows,
                    inference_validated=False, tournament_ready=False)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf8")
    return manifest


def read_package(path):
    path = Path(path)
    manifest = json.loads((path / "manifest.json").read_text(encoding="utf8"))
    binary = path / "weights.bin"
    expected_version = {SCHEMA: 1,
                        "protodd-whole-game-multislot-weights-v2": 2}.get(manifest["schema"])
    if expected_version is None or digest(binary) != manifest["weights_sha256"]:
        raise ValueError("weight package integrity failure")
    with binary.open("rb") as stream:
        if stream.read(8) != MAGIC:
            raise ValueError("weight package magic mismatch")
        version, width, components, count = struct.unpack("<IIII", stream.read(16))
        if (version, width, components, count) != (expected_version, manifest["width"],
                manifest["mixture_components"], len(manifest["tensors"])):
            raise ValueError("weight package architecture mismatch")
        state = {}
        for expected in manifest["tensors"]:
            length, rank = struct.unpack("<HB", stream.read(3))
            name = stream.read(length).decode("ascii")
            shape = [struct.unpack("<I", stream.read(4))[0] for _ in range(rank)]
            if name != expected["name"] or shape != expected["shape"]:
                raise ValueError("weight package tensor metadata mismatch")
            count = math_prod(shape)
            raw = stream.read(count * 4)
            if len(raw) != count * 4:
                raise ValueError("truncated weight tensor")
            state[name] = torch.frombuffer(bytearray(raw), dtype=torch.float32).clone().reshape(shape)
        if stream.read(1):
            raise ValueError("trailing weight bytes")
    return manifest, state


def math_prod(dimensions):
    result = 1
    for dimension in dimensions:
        result *= dimension
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    result = package(args.checkpoint, args.output)
    print(json.dumps(dict(output=str(args.output.resolve()), width=result["width"],
                          parameters=result["parameters"],
                          weights_sha256=result["weights_sha256"]), indent=2))


if __name__ == "__main__":
    main()
