"""Experimental entity/spatial/recurrent Protoss policy for whole-game imitation.

This is an offline teacher. The tournament DLL cannot execute it yet; a bounded
CPU student and action arbiter require separate parity and gameplay validation.
"""
from __future__ import annotations

import math

import torch
from torch import nn
from torch.nn import functional as F


KINDS = ("right_click", "move", "attack", "attack_move", "patrol", "follow", "repair",
         "gather", "rally", "load", "unload_position", "cast", "build", "train", "morph",
         "research", "upgrade", "cancel_queue", "cancel_build", "cancel_morph",
         "cancel_research", "cancel_upgrade", "cancel_addon", "stop", "return_cargo",
         "cloak", "decloak", "siege", "unsiege", "train_fighter", "unload_all",
         "unload_unit", "merge_archon", "merge_dark_archon", "hold", "burrow",
         "unburrow", "cancel_nuke", "liftoff", "stim", "order")
DOMAINS = ("unit_control", "production", "economy", "ability", "transport")
TARGET_MODES = ("none", "entity", "position")
ENTITY_NUMERIC = 16
FORECAST_HORIZONS = (24, 240)
FORECAST_BINARY_TARGETS = ("exploration", "new_enemy", "own_damage", "technology",
                           "enemy_visible", *("command_" + domain for domain in DOMAINS))


class WholeGameModel(nn.Module):
    """Causal memory with conditional action, argument, actor and target heads."""

    def __init__(self, width=256, mixture_components=12):
        super().__init__()
        if width < 64 or mixture_components < 1:
            raise ValueError("invalid model size")
        self.width = width
        self.mixture_components = mixture_components
        self.unit_type = nn.Embedding(256, 32)
        self.relation = nn.Embedding(3, 8)
        self.order = nn.Embedding(256, 16)
        self.entity = nn.Sequential(nn.Linear(ENTITY_NUMERIC + 56, width), nn.LayerNorm(width),
                                    nn.GELU(), nn.Linear(width, width), nn.GELU())
        self.terrain = nn.Sequential(nn.Conv2d(3, 32, 5, stride=2, padding=2), nn.GELU(),
                                     nn.Conv2d(32, 64, 3, stride=2, padding=1), nn.GELU(),
                                     nn.AdaptiveAvgPool2d(1), nn.Flatten())
        # Four complete own-tech arrays plus economy and elapsed time.
        self.global_state = nn.Sequential(nn.Linear(44 * 2 + 61 * 2 + 6, width),
                                          nn.LayerNorm(width), nn.GELU())
        self.query = nn.Linear(width + 64, width)
        self.fusion = nn.Sequential(nn.Linear(width * 2 + 64, width), nn.LayerNorm(width), nn.GELU())
        self.memory = nn.GRUCell(width, width)
        self.event = nn.Linear(width, 1)
        # Player-observable future-state auxiliaries. These are training-only
        # targets; future observations never enter the policy encoder.
        self.forecast = nn.ModuleDict({str(h): nn.Linear(width, len(FORECAST_BINARY_TARGETS) + 1)
                                       for h in FORECAST_HORIZONS})
        self.domain = nn.Linear(width, len(DOMAINS))
        self.kind = nn.Linear(width, len(KINDS))
        self.queued = nn.Linear(width, 2)
        self.target_mode = nn.Linear(width, len(TARGET_MODES))
        self.argument_heads = nn.ModuleDict({
            "order": nn.Linear(width, 256), "unit_type": nn.Linear(width, 256),
            "technology": nn.Linear(width, 44), "upgrade": nn.Linear(width, 61),
            "queue_slot": nn.Linear(width, 16),
        })
        self.actor_key = nn.Linear(width, width)
        self.target_key = nn.Linear(width, width)
        self.actor_query = nn.Linear(width, width)
        self.target_query = nn.Linear(width, width)
        self.position = nn.Linear(width, mixture_components * 5)

    def forward(self, batch, memory=None):
        """One causal observation step. Caller owns/reset/detaches recurrent state."""
        types = batch["type"].clamp(0, 255)
        relation = batch["relation"].clamp(0, 2)
        orders = batch["order"].clamp(0, 255)
        entity = self.entity(torch.cat((self.unit_type(types), self.relation(relation),
                                        self.order(orders), batch["entity_numeric"]), dim=-1))
        mask = batch["entity_mask"]
        if not mask.any(dim=1).all():
            raise ValueError("every observation needs at least one legal entity")
        spatial = self.terrain(batch["spatial"])
        global_state = self.global_state(batch["global"])
        query = self.query(torch.cat((global_state, spatial), dim=-1))
        score = (entity * query[:, None, :]).sum(-1) / math.sqrt(self.width)
        attention = F.softmax(score.masked_fill(~mask, -torch.inf), dim=-1)
        pooled = (attention[..., None] * entity).sum(dim=1)
        observation = self.fusion(torch.cat((global_state, spatial, pooled), dim=-1))
        if memory is None:
            memory = observation.new_zeros(observation.shape)
        state = self.memory(observation, memory)
        scale = math.sqrt(self.width)
        actor = (self.actor_key(entity) * self.actor_query(state)[:, None, :]).sum(-1) / scale
        target = (self.target_key(entity) * self.target_query(state)[:, None, :]).sum(-1) / scale
        return dict(memory=state, event=self.event(state).squeeze(-1), domain=self.domain(state),
                    kind=self.kind(state), queued=self.queued(state), target_mode=self.target_mode(state),
                    **{f"forecast_{h}": head(state) for h, head in self.forecast.items()},
                    actor=actor.masked_fill(~mask, -torch.inf),
                    target=target.masked_fill(~mask, -torch.inf),
                    position=self.position(state).reshape(-1, self.mixture_components, 5),
                    **{name: head(state) for name, head in self.argument_heads.items()})


def position_log_probability(parameters, xy):
    """Mixture density on normalized map coordinates; modes remain multimodal."""
    logits = parameters[..., 0]
    means = parameters[..., 1:3].sigmoid()
    scale = F.softplus(parameters[..., 3:5]).clamp(min=0.01, max=1.0)
    z = (xy[:, None, :] - means) / scale
    log_prob = -0.5 * z.square().sum(-1) - scale.log().sum(-1) - math.log(2 * math.pi)
    return torch.logsumexp(F.log_softmax(logits, -1) + log_prob, -1)


def masked_action_loss(output, label):
    """Only supervise heads backed by an observed command-state transition."""
    pieces = {}
    for name in ("domain", "kind", "queued", "target_mode", "order",
                 "unit_type", "technology", "upgrade", "queue_slot"):
        if label["mask"].get(name, False):
            pieces[name] = F.cross_entropy(output[name], label[name])
    if label["mask"].get("actors", False):
        valid = label["actor_known"] & torch.isfinite(output["actor"])
        if valid.any():
            pieces["actors"] = F.binary_cross_entropy_with_logits(
                output["actor"][valid], label["actor_positive"][valid].float())
    if label["mask"].get("target_entity", False):
        if (label["target_entity"] < 0).any():
            raise ValueError("unresolved target may not train the entity pointer")
        pieces["target_entity"] = F.cross_entropy(output["target"], label["target_entity"])
    if label["mask"].get("target_position", False):
        pieces["target_position"] = -position_log_probability(
            output["position"], label["target_position"]).mean()
    if not pieces:
        raise ValueError("no supervised action head")
    return sum(pieces.values()), {key: value.detach() for key, value in pieces.items()}
