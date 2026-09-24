"""Package a frozen six-slot teacher for the compiled tournament decoder.

Export is only an interchange step. Numerical parity, live latency and paired
strength gates must pass before embedding these weights in the control DLL.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import struct

import torch

from .whole_game_export import MAGIC, digest
from .whole_game_multislot_model import MultiSlotWholeGameModel


SCHEMA = "protodd-whole-game-multislot-weights-v2"
CHECKPOINT_SCHEMA = "protodd-whole-game-multislot-fit-v1"


def package(checkpoint_path, output):
    checkpoint_path, output = Path(checkpoint_path), Path(output)
    saved = torch.load(checkpoint_path, map_location="cpu", weights_only=True)
    if saved.get("schema") != CHECKPOINT_SCHEMA:
        raise ValueError("unsupported multi-slot checkpoint")
    spec = json.loads((checkpoint_path.parent / "run.json").read_text(encoding="utf8"))
    if (spec.get("schema") != CHECKPOINT_SCHEMA or spec.get("maximum_slots") != 6 or
            spec.get("training_identity_sha256") != saved.get("source_identity_sha256")):
        raise ValueError("checkpoint is not the frozen six-slot training run")
    state = saved["state_dict"]
    width = int(state["memory.weight_hh"].shape[1])
    components = int(state["position.weight"].shape[0] // 5)
    if width < 64 or width > 2048 or components < 1 or components > 64:
        raise ValueError("unsupported multi-slot architecture")
    # A fixed six-slot output contract is required by the compiled scheduler.
    model = MultiSlotWholeGameModel(width=width, mixture_components=components,
                                    maximum_slots=6)
    model.load_state_dict(state, strict=True)
    if output.exists():
        raise FileExistsError(output)
    output.mkdir(parents=True)
    rows = []
    binary = output / "weights.bin"
    with binary.open("wb") as stream:
        stream.write(MAGIC)
        stream.write(struct.pack("<III", 2, width, components))
        ordered = sorted(state.items())
        stream.write(struct.pack("<I", len(ordered)))
        for name, tensor in ordered:
            data = tensor.detach().cpu().contiguous().float().numpy().astype("<f4", copy=False)
            if not bool(torch.isfinite(tensor).all()):
                raise ValueError(f"non-finite parameter: {name}")
            encoded = name.encode("ascii")
            if len(encoded) > 128 or data.ndim > 4:
                raise ValueError("unsupported parameter name or rank")
            stream.write(struct.pack("<HB", len(encoded), data.ndim))
            stream.write(encoded)
            for dimension in data.shape:
                stream.write(struct.pack("<I", dimension))
            stream.write(data.tobytes(order="C"))
            rows.append(dict(name=name, shape=list(data.shape), elements=int(data.size)))
    manifest = dict(schema=SCHEMA, checkpoint_sha256=digest(checkpoint_path),
                    run_sha256=digest(checkpoint_path.parent / "run.json"),
                    weights_sha256=digest(binary),
                    source_identity_sha256=saved["source_identity_sha256"],
                    width=width, mixture_components=components, maximum_slots=6,
                    parameters=sum(row["elements"] for row in rows), tensors=rows,
                    inference_validated=False, tournament_ready=False)
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                           encoding="utf8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(package(args.checkpoint, args.output), indent=2))


if __name__ == "__main__":
    main()
