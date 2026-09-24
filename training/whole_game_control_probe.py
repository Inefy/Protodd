"""Create a conspicuously synthetic policy to test compiled command execution.

The probe always proposes owned-unit moves to the map centre. It is only for
development games and must never be assessed or submitted as a playing model.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path

import torch

from .whole_game_export import package
from .whole_game_model import KINDS, TARGET_MODES


def make_probe(source_checkpoint, output):
    source_checkpoint, output = Path(source_checkpoint), Path(output)
    if output.exists():
        raise FileExistsError(output)
    original = torch.load(source_checkpoint, map_location="cpu", weights_only=True)
    if original.get("schema") != "protodd-whole-game-fit-v1":
        raise ValueError("unsupported source checkpoint")
    state = {name: tensor.clone() for name, tensor in original["state_dict"].items()}
    for name in ("event", "kind", "domain", "target_mode", "queued", "position"):
        state[name + ".weight"].zero_()
        state[name + ".bias"].zero_()
    state["event.bias"].fill_(8)
    state["kind.bias"].fill_(-8)
    state["kind.bias"][KINDS.index("move")] = 8
    state["domain.bias"][0] = 8
    state["target_mode.bias"].fill_(-8)
    state["target_mode.bias"][TARGET_MODES.index("position")] = 8
    state["queued.bias"][0] = 8
    state["position.bias"][0::5] = -8
    state["position.bias"][0] = 8
    for name in ("actor_key", "actor_query"):
        state[name + ".weight"].zero_()
        state[name + ".bias"].fill_(1)
    output.mkdir(parents=True)
    checkpoint = output / "probe.pt"
    torch.save(dict(schema="protodd-whole-game-fit-v1", state_dict=state,
                    source_identity_sha256=original["source_identity_sha256"],
                    diagnostic_only=True), checkpoint)
    exported = package(checkpoint, output / "export")
    record = dict(schema="protodd-whole-game-control-probe-v1", diagnostic_only=True,
                  behavior="owned-unit move to map centre on every model tick",
                  source_checkpoint_sha256=hashlib.sha256(source_checkpoint.read_bytes()).hexdigest(),
                  probe_checkpoint_sha256=hashlib.sha256(checkpoint.read_bytes()).hexdigest(),
                  weights_sha256=exported["weights_sha256"],
                  weights=str((output / "export" / "weights.bin").resolve()))
    (output / "probe.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf8")
    return record


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source_checkpoint", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(make_probe(args.source_checkpoint, args.output), indent=2))


if __name__ == "__main__":
    main()
