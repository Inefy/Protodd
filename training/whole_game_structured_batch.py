"""Experimental minibatches with actor-balanced, compatible action supervision."""
from __future__ import annotations

import torch
from torch.nn import functional as F

from .whole_game_batch import _task, collate_observations, minibatch_loss
from .whole_game_features import encode_label
from .whole_game_structured_loss import structured_action_loss


def structured_minibatch_loss(model, samples, device, cache=None):
    if not samples:
        raise ValueError("empty minibatch")
    if _task(samples[0]) != "action":
        return minibatch_loss(model, samples, device, cache)
    if any(_task(sample) != "action" for sample in samples):
        raise ValueError("minibatch must share task")
    if len({tuple(sample[3].shape) for sample in samples}) != 1:
        raise ValueError("minibatch maps must have equal spatial dimensions")
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
        memory = torch.where(torch.tensor(valid, dtype=torch.bool, device=device)[:, None],
                             proposed, previous)
    batch, ids, _ = collate_observations([sample[1] for sample in samples], terrains, device, cache)
    output = model(batch, memory)
    losses = []
    for index, sample in enumerate(samples):
        terrain = sample[3]
        label = encode_label(sample[2]["action"], ids[index], terrain.shape[1], terrain.shape[0])
        for name in ("actor_known", "actor_positive"):
            label[name] = F.pad(label[name], (0, output["actor"].shape[1] - label[name].shape[1]))
        label = {name: value.to(device) if isinstance(value, torch.Tensor) else value
                 for name, value in label.items()}
        single = {name: value[index:index + 1] for name, value in output.items()}
        loss, _ = structured_action_loss(single, label)
        losses.append(loss)
    return torch.stack(losses).mean()
