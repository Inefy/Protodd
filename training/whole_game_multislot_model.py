"""One-encoder, ordered command decoder for the GPU whole-game teacher.

At a cadence observation the backbone updates game memory once. Each command
slot chooses an owned actor, a command conditioned on that actor, arguments and
a relative dispatch frame. The next slot depends on only earlier slot tokens.
Teacher tokens are output history, never future observation inputs.
"""
from __future__ import annotations

import math

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_action_schema import KIND_TARGET_MODES
from .whole_game_model import KINDS, TARGET_MODES, WholeGameModel


class MultiSlotWholeGameModel(WholeGameModel):
    def __init__(self, width=512, mixture_components=12, maximum_slots=6):
        super().__init__(width=width, mixture_components=mixture_components)
        if not 1 <= maximum_slots <= 16:
            raise ValueError("invalid command slot count")
        self.maximum_slots = maximum_slots
        self.slot_stop = nn.Linear(width, 2)
        self.slot_actor_key = nn.Linear(width, width)
        self.slot_actor_query = nn.Linear(width, width)
        self.slot_kind = nn.Linear(width * 2, len(KINDS))
        self.slot_kind_context = nn.Embedding(len(KINDS), 64)
        self.slot_mode = nn.Linear(width * 2 + 64, len(TARGET_MODES))
        self.slot_domain = nn.Linear(width * 2 + 64, self.domain.out_features)
        self.slot_target_key = nn.Linear(width, width)
        self.slot_target_query = nn.Linear(width * 2 + 64, width)
        self.slot_position = nn.Linear(width * 2 + 64, mixture_components * 5)
        self.slot_delay = nn.Linear(width * 2 + 64, 24)
        self.slot_queued = nn.Linear(width * 2 + 64, 2)
        self.slot_arguments = nn.ModuleDict({
            "order": nn.Linear(width * 2 + 64, 256),
            "unit_type": nn.Linear(width * 2 + 64, 256),
            "technology": nn.Linear(width * 2 + 64, 44),
            "upgrade": nn.Linear(width * 2 + 64, 61),
            "queue_slot": nn.Linear(width * 2 + 64, 16),
        })
        self.slot_mode_context = nn.Embedding(len(TARGET_MODES), 16)
        self.slot_delay_context = nn.Embedding(24, 16)
        self.slot_position_context = nn.Linear(2, 32)
        self.slot_unit_context = nn.Embedding(256, 32)
        self.slot_transition = nn.GRUCell(width * 2 + 64 + 16 + 16 + 32 + 32,
                                          width)
        self.register_buffer("supported_kind", torch.tensor(
            [bool(KIND_TARGET_MODES.get(kind)) for kind in KINDS], dtype=torch.bool),
            persistent=False)
        self.register_buffer("supported_mode", torch.tensor(
            [[mode in KIND_TARGET_MODES.get(kind, ()) for mode in TARGET_MODES]
             for kind in KINDS], dtype=torch.bool), persistent=False)

    def _entities(self, batch):
        return self.entity(torch.cat((
            self.unit_type(batch["type"].clamp(0, 255)),
            self.relation(batch["relation"].clamp(0, 2)),
            self.order(batch["order"].clamp(0, 255)),
            batch["entity_numeric"]), dim=-1))

    @staticmethod
    def _choice(teacher, name, logits, mask=None):
        if teacher is not None:
            selected = teacher[name].to(logits.device).long()
            if selected.shape != logits.shape[:1]:
                raise ValueError(f"teacher {name} has wrong batch shape")
            if (selected < 0).any() or (selected >= logits.shape[1]).any():
                raise ValueError(f"teacher {name} is out of range")
            if mask is not None and not mask.gather(1, selected[:, None]).all():
                raise ValueError(f"teacher {name} is masked")
            return selected
        if mask is not None:
            logits = logits.masked_fill(~mask, -torch.inf)
        return logits.argmax(dim=-1)

    def forward_slots(self, batch, memory=None, *, teacher_tokens=None, slots=None):
        count = self.maximum_slots if slots is None else slots
        if not 1 <= count <= self.maximum_slots:
            raise ValueError("invalid decoded slot count")
        if teacher_tokens is not None and len(teacher_tokens) != count:
            raise ValueError("teacher tokens must cover all decoded slots")
        base = super().forward(batch, memory)
        entities = self._entities(batch)
        own = batch["entity_mask"] & (batch["relation"] == 0)
        visible = batch["entity_mask"] & (
            own | (batch["entity_numeric"][..., 4] > 0.5))
        if not own.any(dim=1).all():
            raise ValueError("command decoder requires an owned actor")
        state = base["memory"]
        decoded = []
        scale = math.sqrt(self.width)
        for index in range(count):
            teacher = None if teacher_tokens is None else teacher_tokens[index]
            stop = self.slot_stop(state)
            actor = (self.slot_actor_key(entities) *
                     self.slot_actor_query(state)[:, None, :]).sum(-1) / scale
            actor = actor.masked_fill(~own, -torch.inf)
            actor_index = self._choice(teacher, "actor", actor, own)
            selected_actor = entities.gather(1, actor_index[:, None, None].expand(
                -1, 1, self.width)).squeeze(1)
            actor_state = torch.cat((state, selected_actor), dim=-1)
            kind = self.slot_kind(actor_state)
            kind_index = self._choice(teacher, "kind", kind,
                                      None if teacher is not None else
                                      self.supported_kind[None].expand_as(kind))
            kind_embedding = self.slot_kind_context(kind_index)
            argument_state = torch.cat((actor_state, kind_embedding), dim=-1)
            mode = self.slot_mode(argument_state)
            legal_modes = self.supported_mode[kind_index]
            mode_index = self._choice(teacher, "target_mode", mode,
                                      legal_modes if teacher is None else None)
            target = (self.slot_target_key(entities) *
                      self.slot_target_query(argument_state)[:, None, :]).sum(-1) / scale
            target = target.masked_fill(~visible, -torch.inf)
            delay = self.slot_delay(argument_state)
            delay_index = self._choice(teacher, "delay", delay)
            position = self.slot_position(argument_state).reshape(
                -1, self.mixture_components, 5)
            arguments = {name: head(argument_state) for name, head in
                         self.slot_arguments.items()}
            if teacher is None:
                has_target = mode_index == TARGET_MODES.index("entity")
                target_index = target.argmax(-1)
                has_position = mode_index == TARGET_MODES.index("position")
                components = position[:, :, 0].argmax(-1)
                xy = position[torch.arange(position.shape[0], device=position.device),
                              components, 1:3].sigmoid()
                unit_index = arguments["unit_type"].argmax(-1)
            else:
                target_index = teacher.get("target_entity", torch.full_like(
                    actor_index, -1)).to(actor_index.device).long()
                if target_index.shape != actor_index.shape or (target_index < -1).any() or \
                        (target_index >= entities.shape[1]).any():
                    raise ValueError("teacher target entity is out of range")
                has_target = target_index >= 0
                if has_target.any() and not visible.gather(
                        1, target_index.clamp(min=0)[:, None])[has_target].all():
                    raise ValueError("teacher target entity is not visible")
                xy = teacher.get("position", torch.zeros(
                    (actor_index.shape[0], 2), device=actor_index.device))
                xy = xy.to(actor_index.device).float()
                if xy.shape != (actor_index.shape[0], 2) or not torch.isfinite(xy).all():
                    raise ValueError("teacher target position is invalid")
                has_position = teacher.get("has_position", torch.zeros_like(
                    actor_index, dtype=torch.bool)).to(actor_index.device).bool()
                unit_index = teacher.get("unit_type", torch.zeros_like(
                    actor_index)).to(actor_index.device).long()
                if (unit_index < 0).any() or (unit_index >= 256).any():
                    raise ValueError("teacher unit type is out of range")
            decoded.append(dict(stop=stop, actor=actor, kind=kind,
                                target_mode=mode, target=target,
                                position=position,
                                delay=delay, domain=self.slot_domain(argument_state),
                                queued=self.slot_queued(argument_state),
                                chosen_stop=stop.argmax(-1),
                                chosen_actor=actor_index, chosen_kind=kind_index,
                                chosen_target_mode=mode_index,
                                chosen_target_entity=torch.where(
                                    has_target, target_index,
                                    torch.full_like(target_index, -1)),
                                chosen_position=torch.where(has_position[:, None], xy,
                                                            torch.zeros_like(xy)),
                                chosen_delay=delay_index,
                                chosen_unit_type=unit_index,
                                **arguments))
            target_embedding = entities.gather(1, target_index.clamp(min=0)[
                :, None, None].expand(-1, 1, self.width)).squeeze(1)
            target_embedding = target_embedding * has_target[:, None]
            position_embedding = self.slot_position_context(xy) * has_position[:, None]
            token = torch.cat((selected_actor, kind_embedding,
                               self.slot_mode_context(mode_index),
                               self.slot_delay_context(delay_index),
                               target_embedding, position_embedding,
                               self.slot_unit_context(unit_index)), dim=-1)
            state = self.slot_transition(token, state)
        return dict(backbone=base, slots=decoded)
