"""Experimental group command policy with relational inputs and discrete positions.

No native export or control authorization exists for this architecture.
The 32x32 coarse cell and 4x4 subcell share the same decoding rule as training.
"""
from __future__ import annotations

import math

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_model import KINDS, TARGET_MODES
from .whole_game_multislot_model import MultiSlotWholeGameModel


def position_classes(xy):
    fine_grid = (xy.clamp(0, 1 - 1e-7) * 128).long()
    coarse = fine_grid // 4
    sub = fine_grid % 4
    return coarse[:, 1] * 32 + coarse[:, 0], sub[:, 1] * 4 + sub[:, 0]


def class_position(coarse, sub):
    x = (coarse % 32) * 4 + sub % 4
    y = (coarse // 32) * 4 + sub // 4
    return (torch.stack((x, y), -1).float() + 0.5) / 128


class GroupCommandModel(MultiSlotWholeGameModel):
    def __init__(self, width=256, maximum_slots=6, max_actors=12):
        super().__init__(width=width, maximum_slots=maximum_slots)
        self.max_actors = max_actors
        self.rel_query = nn.Linear(width, 32)
        self.rel_key = nn.Linear(width, 32)
        self.rel_value = nn.Linear(width, width)
        self.rel_norm = nn.LayerNorm(width)
        self.local_map = nn.Linear(64, width)
        self.group_count = nn.Linear(width, max_actors)
        argument_width = width * 2 + 64
        self.position_cell = nn.Linear(argument_width, 1024)
        self.cell_context = nn.Embedding(1024, 32)
        self.position_subcell = nn.Linear(argument_width + 32, 16)
        self.teacher_probability = 1.0

    def _entities(self, batch, spatial=None):
        raw = super()._entities(batch)
        if spatial is None:
            spatial = self.terrain[:4](batch["spatial"])
        xy = batch["entity_numeric"][..., :2].clamp(0, 1 - 1e-7)
        x, y = (xy[..., 0] * spatial.shape[-1]).long(), (xy[..., 1] * spatial.shape[-2]).long()
        local = spatial.flatten(2).transpose(1, 2).gather(1,
            (y * spatial.shape[-1] + x)[..., None].expand(-1, -1, 64))
        raw = raw + self.local_map(local)
        logits = torch.bmm(self.rel_query(raw), self.rel_key(raw).transpose(1, 2)) / math.sqrt(32)
        displacement = xy[:, :, None] - xy[:, None, :]
        logits = logits - displacement.square().sum(-1) * 32
        weights = logits.masked_fill(~batch["entity_mask"][:, None], -torch.inf).softmax(-1)
        return self.rel_norm(raw + torch.bmm(weights, self.rel_value(raw)))

    def encode_step(self, batch):
        spatial_features = self.terrain[:4](batch["spatial"])
        entities = self._entities(batch, spatial_features)
        spatial = self.terrain[4:](spatial_features)
        global_state = self.global_state(batch["global"])
        query = self.query(torch.cat((global_state, spatial), -1))
        attention = ((entities * query[:, None]).sum(-1) / math.sqrt(self.width)).masked_fill(
            ~batch["entity_mask"], -torch.inf).softmax(-1)
        pooled = (entities * attention[..., None]).sum(1)
        observation = self.fusion(torch.cat((global_state, spatial, pooled), -1))
        return observation, entities

    def forward(self, batch, memory=None):
        observation, entities = self.encode_step(batch)
        if memory is None:
            memory = torch.zeros_like(observation)
        return dict(memory=self.memory(observation, memory), entities=entities)

    def forward_slots(self, batch, memory=None, *, teacher_tokens=None, slots=None, encoded_base=None):
        count = self.maximum_slots if slots is None else slots
        if not 1 <= count <= self.maximum_slots:
            raise ValueError("invalid slot count")
        base = self.forward(batch, memory) if encoded_base is None else encoded_base
        state, entities = base["memory"], base["entities"]
        own = batch["entity_mask"] & (batch["relation"] == 0)
        visible = batch["entity_mask"] & (own | (batch["entity_numeric"][..., 4] > .5))
        if not own.any(1).all():
            raise ValueError("no owned actor")
        decoded = []
        for index in range(count):
            teacher = None if teacher_tokens is None else teacher_tokens[index]
            use_teacher = torch.zeros(state.shape[0], dtype=torch.bool, device=state.device)
            if teacher is not None:
                probability = self.teacher_probability if self.training else 1.0
                use_teacher = torch.rand(state.shape[0], device=state.device) < probability
            def choose(name, prediction):
                return torch.where(use_teacher, teacher[name].to(prediction.device), prediction) \
                    if teacher is not None else prediction
            stop = self.slot_stop(state)
            actor = (self.slot_actor_key(entities) * self.slot_actor_query(state)[:, None]).sum(-1) / math.sqrt(self.width)
            actor = actor.masked_fill(~own, -torch.inf)
            cardinality = self.group_count(state)
            size = torch.minimum(cardinality.argmax(-1) + 1, own.sum(-1))
            ranks = actor.argsort(-1, descending=True).argsort(-1)
            predicted_set = (ranks < size[:, None]) & own
            actor_set = predicted_set
            if teacher is not None:
                gold = teacher["actor_set"].to(own.device)
                if gold.shape != own.shape or (gold & ~own).any() or not gold.any(-1).all():
                    raise ValueError("invalid teacher actor set")
                actor_set = torch.where(use_teacher[:, None], gold, predicted_set)
            selected_actor = (entities * actor_set[..., None]).sum(1) / actor_set.sum(1, keepdim=True)
            actor_state = torch.cat((state, selected_actor), -1)
            kind = self.slot_kind(actor_state)
            kind_index = choose("kind", kind.masked_fill(~self.supported_kind, -torch.inf).argmax(-1))
            kind_embedding = self.slot_kind_context(kind_index)
            argument_state = torch.cat((actor_state, kind_embedding), -1)
            mode = self.slot_mode(argument_state)
            mode_index = choose("target_mode", mode.masked_fill(~self.supported_mode[kind_index], -torch.inf).argmax(-1))
            target = (self.slot_target_key(entities) * self.slot_target_query(argument_state)[:, None]).sum(-1) / math.sqrt(self.width)
            target = target.masked_fill(~visible, -torch.inf)
            has_target = mode_index == TARGET_MODES.index("entity")
            target_index = target.argmax(-1)
            if teacher is not None:
                target_index = choose("target_entity", target_index)
                has_target = torch.where(use_teacher, target_index >= 0, has_target)
            delay = self.slot_delay(argument_state)
            delay_index = choose("delay", delay.argmax(-1))
            cell = self.position_cell(argument_state)
            cell_index = cell.argmax(-1)
            if teacher is not None:
                gold_cell, _ = position_classes(teacher["position"].to(state.device))
                cell_index = torch.where(use_teacher, gold_cell, cell_index)
            subcell = self.position_subcell(torch.cat((argument_state, self.cell_context(cell_index)), -1))
            predicted_xy = class_position(cell_index, subcell.argmax(-1))
            xy = choose("position", predicted_xy) if teacher is None else torch.where(
                use_teacher[:, None], teacher["position"].to(state.device), predicted_xy)
            has_position = mode_index == TARGET_MODES.index("position")
            if teacher is not None:
                has_position = torch.where(use_teacher, teacher["has_position"].to(state.device), has_position)
            arguments = {name: head(argument_state) for name, head in self.slot_arguments.items()}
            unit_index = choose("unit_type", arguments["unit_type"].argmax(-1))
            decoded.append(dict(stop=stop, actor=actor, actor_count=cardinality, kind=kind,
                target_mode=mode, target=target, delay=delay, position_cell=cell, position_subcell=subcell,
                domain=self.slot_domain(argument_state), queued=self.slot_queued(argument_state),
                chosen_stop=stop.argmax(-1), chosen_actor=actor.masked_fill(~actor_set, -torch.inf).argmax(-1),
                chosen_actor_set=actor_set, chosen_kind=kind_index, chosen_target_mode=mode_index,
                chosen_target_entity=torch.where(has_target, target_index, torch.full_like(target_index, -1)),
                chosen_position=torch.where(has_position[:, None], xy, torch.zeros_like(xy)),
                chosen_delay=delay_index, chosen_unit_type=unit_index, **arguments))
            target_embedding = entities.gather(1, target_index.clamp(min=0)[:, None, None].expand(-1, 1, self.width)).squeeze(1) * has_target[:, None]
            token = torch.cat((selected_actor, kind_embedding, self.slot_mode_context(mode_index),
                self.slot_delay_context(delay_index), target_embedding,
                self.slot_position_context(xy) * has_position[:, None], self.slot_unit_context(unit_index)), -1)
            state = self.slot_transition(token, state)
        return dict(backbone=base, slots=decoded)
