"""Actor-conditioned whole-game teacher with a baseline-compatible output API.

The GPU model is an architecture experiment. Its added heads start as zero
residuals on a verified baseline checkpoint, so a comparison is attributable
to learned conditioning rather than a random initial action policy.
"""
from __future__ import annotations

import math

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_model import KINDS, WholeGameModel


class ConditionalWholeGameModel(WholeGameModel):
    def __init__(self, width=512, mixture_components=12):
        super().__init__(width=width, mixture_components=mixture_components)
        self.conditional_kind = nn.Linear(width * 2, len(KINDS))
        self.kind_context = nn.Embedding(len(KINDS), 64)
        self.conditional_mode = nn.Linear(width * 2 + 64, 3)
        self.conditional_unit_type = nn.Linear(width * 2 + 64, 256)
        self.conditional_target_query = nn.Linear(width * 2 + 64, width)
        self.conditional_position = nn.Linear(width * 2 + 64, mixture_components * 5)
        for layer in (self.conditional_kind, self.conditional_mode,
                      self.conditional_unit_type, self.conditional_target_query,
                      self.conditional_position):
            nn.init.zeros_(layer.weight)
            nn.init.zeros_(layer.bias)

    def forward(self, batch, memory=None):
        result = super().forward(batch, memory)
        types = batch["type"].clamp(0, 255)
        relations = batch["relation"].clamp(0, 2)
        orders = batch["order"].clamp(0, 255)
        entities = self.entity(torch.cat((self.unit_type(types),
                                          self.relation(relations),
                                          self.order(orders),
                                          batch["entity_numeric"]), dim=-1))
        own = batch["entity_mask"] & (batch["relation"] == 0)
        if not own.any(dim=1).all():
            raise ValueError("conditional policy needs an owned actor")
        actor = result["actor"].masked_fill(~own, -torch.inf)
        probabilities = F.softmax(actor, dim=1)
        chosen = (probabilities[..., None] * entities).sum(dim=1)
        state_actor = torch.cat((result["memory"], chosen), dim=-1)
        kind = result["kind"] + self.conditional_kind(state_actor)
        kind_embedding = F.softmax(kind, dim=-1) @ self.kind_context.weight
        arguments = torch.cat((state_actor, kind_embedding), dim=-1)
        conditioned_target = self.conditional_target_query(arguments)
        target_delta = (self.target_key(entities) * conditioned_target[:, None, :]).sum(-1)
        target_delta = target_delta / math.sqrt(self.width)
        result["actor"] = actor
        result["kind"] = kind
        result["target_mode"] = result["target_mode"] + self.conditional_mode(arguments)
        result["unit_type"] = result["unit_type"] + self.conditional_unit_type(arguments)
        result["target"] = (result["target"] + target_delta).masked_fill(
            ~batch["entity_mask"], -torch.inf)
        result["position"] = (result["position"] +
                              self.conditional_position(arguments).reshape(
                                  -1, self.mixture_components, 5))
        return result


def initialize_from_baseline(model, checkpoint_path, train_identity_sha):
    """Load the exact baseline backbone and reject missing/mismatched weights."""
    from pathlib import Path
    import hashlib

    checkpoint_path = Path(checkpoint_path)
    previous = torch.load(checkpoint_path, map_location="cpu", weights_only=True)
    if previous.get("source_identity_sha256") != train_identity_sha:
        raise ValueError("initial checkpoint belongs to another replay release")
    expected = WholeGameModel(width=model.width,
                              mixture_components=model.mixture_components)
    expected.load_state_dict(previous["state_dict"], strict=True)
    incompatible = model.load_state_dict(previous["state_dict"], strict=False)
    if incompatible.unexpected_keys or set(incompatible.missing_keys) != {
            name for name in model.state_dict() if name not in expected.state_dict()}:
        raise ValueError("conditional checkpoint has incompatible backbone")
    return hashlib.sha256(checkpoint_path.read_bytes()).hexdigest()
