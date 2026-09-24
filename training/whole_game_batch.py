"""Padded minibatches for causal whole-game GPU teacher training.

Samples in one minibatch share map dimensions and supervision task. Entity
padding never contributes to attention, actor loss or target pointers.
"""
from __future__ import annotations

import math

import torch
from torch.nn import functional as F

from .whole_game_features import encode_label, encode_observation
from .whole_game_model import FORECAST_BINARY_TARGETS, masked_action_loss


ENTITY_FIELDS = ("type", "relation", "order", "entity_numeric", "entity_mask")


def collate_observations(rows, terrains, device, cache=None):
    if not rows or len(rows) != len(terrains):
        raise ValueError("nonempty aligned observations and terrain required")
    encoded = [(cache.get(row, terrain) if cache is not None else encode_observation(row, terrain))
               for row, terrain in zip(rows, terrains)]
    spatial_shapes = {tuple(batch["spatial"].shape[-2:]) for batch, _, _ in encoded}
    if len(spatial_shapes) != 1:
        raise ValueError("minibatch maps must have equal spatial dimensions")
    width = max(batch["type"].shape[1] for batch, _, _ in encoded)
    result = {}
    for field in encoded[0][0]:
        values = []
        for batch, _, _ in encoded:
            value = batch[field]
            if field in ENTITY_FIELDS:
                padding = width - value.shape[1]
                value = F.pad(value, (0, 0, 0, padding)) if value.ndim == 3 else F.pad(value, (0, padding))
            values.append(value)
        result[field] = torch.cat(values, dim=0).to(device)
    return result, [ids for _, ids, _ in encoded], [overflow for _, _, overflow in encoded]


def _task(sample):
    target = sample[2]
    return "forecast" if "forecast" in target else "event" if target["update_memory"] else "action"


def minibatch_loss(model, samples, device, cache=None):
    """One batched forward history and a mean masked objective."""
    if not samples:
        raise ValueError("empty minibatch")
    tasks = {_task(sample) for sample in samples}
    shapes = {tuple(sample[3].shape) for sample in samples}
    if len(tasks) != 1 or len(shapes) != 1:
        raise ValueError("minibatch must share task and map dimensions")
    terrains = [sample[3] for sample in samples]
    max_history = max(len(sample[0]) for sample in samples)
    memory = None
    for step in range(max_history):
        valid = [step >= max_history - len(sample[0]) for sample in samples]
        rows = [sample[0][step - (max_history - len(sample[0]))] if active else sample[1]
                for sample, active in zip(samples, valid)]
        batch, _, _ = collate_observations(rows, terrains, device, cache)
        proposed = model(batch, memory)["memory"]
        previous = torch.zeros_like(proposed) if memory is None else memory
        mask = torch.tensor(valid, dtype=torch.bool, device=device)[:, None]
        memory = torch.where(mask, proposed, previous)
    batch, ids, _ = collate_observations([sample[1] for sample in samples], terrains, device, cache)
    output = model(batch, memory)
    task = next(iter(tasks))
    if task == "event":
        truth = torch.tensor([sample[2]["event"] for sample in samples],
                             dtype=torch.float32, device=device)
        return F.binary_cross_entropy_with_logits(output["event"], truth)
    if task == "forecast":
        horizons = {sample[2]["horizon"] for sample in samples}
        if len(horizons) != 1:
            raise ValueError("forecast minibatch needs one horizon")
        prediction = output[f"forecast_{next(iter(horizons))}"]
        truths, magnitudes = [], []
        for sample in samples:
            future = sample[2]["forecast"]
            truth = [int(future["newly_explored_tiles"] > 0),
                     int(future["newly_known_enemy_ids_retained"] > 0),
                     int(future["own_hp_shield_loss_common"] > 0),
                     int(future["technology_completed_gain"] + future["upgrade_level_gain"] > 0),
                     int(future["visible_enemies_at_target"] > 0),
                     *future["confirmed_command_domains"]]
            if len(truth) != len(FORECAST_BINARY_TARGETS):
                raise ValueError("forecast target width changed")
            truths.append(truth)
            magnitudes.append(math.log1p(future["newly_explored_tiles"]) /
                              math.log1p(len(sample[1]["vision"])))
        binary = torch.tensor(truths, dtype=torch.float32, device=device)
        magnitude = torch.tensor(magnitudes, dtype=torch.float32, device=device)
        return (F.binary_cross_entropy_with_logits(prediction[:, :-1], binary) +
                F.smooth_l1_loss(prediction[:, -1], magnitude))
    losses = []
    for index, sample in enumerate(samples):
        terrain = sample[3]
        label = encode_label(sample[2]["action"], ids[index], terrain.shape[1], terrain.shape[0])
        for name in ("actor_known", "actor_positive"):
            label[name] = F.pad(label[name], (0, output["actor"].shape[1] - label[name].shape[1]))
        label = {name: value.to(device) if isinstance(value, torch.Tensor) else value
                 for name, value in label.items()}
        single = {name: value[index:index + 1] for name, value in output.items()}
        loss, _ = masked_action_loss(single, label)
        losses.append(loss)
    return torch.stack(losses).mean()
