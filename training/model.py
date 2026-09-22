"""Configurable MLP with the same bounded operators as LearnedPolicy.cpp."""
from pathlib import Path
import struct

import numpy as np
import torch
from torch import nn

from .schema import load_schema

MAGIC = b"PTDMLP1\n"
HEADER = struct.Struct("<8sIQIIII")
MAX_WIDTH = 2048
MAX_PARAMETERS = 8_000_000


class FrequencyBaseline(nn.Module):
    """Training-only action frequencies, masked with the same candidate set."""
    def __init__(self, frequencies):
        super().__init__()
        prior = torch.tensor(frequencies, dtype=torch.float32) + 1e-3
        self.register_buffer("log_prior", (prior / prior.sum()).log())

    def forward(self, features):
        return self.log_prior[None, :].expand(features.shape[0], -1)


class MacroModel(nn.Module):
    def __init__(self, hidden=(1024, 1024)):
        super().__init__()
        schema = load_schema()
        if len(hidden) != 2 or any(type(v) is not int or not 1 <= v <= MAX_WIDTH for v in hidden):
            raise ValueError("exactly two hidden widths between 1 and 2048 required")
        self.hidden = tuple(hidden)
        self.layers = nn.Sequential(nn.Linear(len(schema["features"]), hidden[0]), nn.ReLU(),
                                    nn.Linear(hidden[0], hidden[1]), nn.ReLU(),
                                    nn.Linear(hidden[1], len(schema["actions"])))
        if sum(p.numel() for p in self.parameters()) > MAX_PARAMETERS:
            raise ValueError("model parameter limit exceeded")

    def forward(self, features):
        return self.layers(features)


def masked_logits(logits, masks):
    if masks.shape != logits.shape or masks.dtype != torch.bool or not masks.any(dim=1).all():
        raise ValueError("invalid or empty action masks")
    return logits.masked_fill(~masks, -torch.inf)


def export_model(model, path):
    schema = load_schema()
    header = HEADER.pack(MAGIC, 1, int(schema["fingerprint"], 16), len(schema["features"]),
                         *model.hidden, len(schema["actions"]))
    arrays = [p.detach().cpu().numpy().astype("<f4").ravel() for p in model.parameters()]
    weights = np.concatenate(arrays)
    if not np.isfinite(weights).all() or (np.abs(weights) > 1000).any():
        raise ValueError("cannot export nonfinite or excessive weights")
    Path(path).write_bytes(header + weights.tobytes())


def load_model(path):
    schema = load_schema()
    path = Path(path)
    if path.stat().st_size > MAX_PARAMETERS * 4 + HEADER.size:
        raise ValueError("model exceeds size limit")
    raw = path.read_bytes()
    if len(raw) < HEADER.size:
        raise ValueError("truncated model header")
    magic, version, fingerprint, inputs, h1, h2, outputs = HEADER.unpack_from(raw)
    if (magic != MAGIC or version != 1 or fingerprint != int(schema["fingerprint"], 16) or
            inputs != len(schema["features"]) or outputs != len(schema["actions"])):
        raise ValueError("model schema mismatch")
    model = MacroModel((h1, h2))
    count = sum(p.numel() for p in model.parameters())
    if len(raw) != HEADER.size + count * 4:
        raise ValueError("model weight length mismatch")
    weights = np.frombuffer(raw, dtype="<f4", offset=HEADER.size)
    if not np.isfinite(weights).all() or (np.abs(weights) > 1000).any():
        raise ValueError("invalid weights")
    offset = 0
    with torch.no_grad():
        for parameter in model.parameters():
            size = parameter.numel()
            parameter.copy_(torch.from_numpy(weights[offset:offset + size].copy()).reshape(parameter.shape))
            offset += size
    return model.eval()
