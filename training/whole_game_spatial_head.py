"""Experimental map-location head that retains a 16x16 spatial layout.

The current global Gaussian head compresses terrain and vision to one average.
This head scores map cells conditioned on memory, command kind and actor
position, then predicts a within-cell offset. It is not yet in the Win32 bot.
"""
from __future__ import annotations

import math

import torch
from torch import nn
from torch.nn import functional as F

from .whole_game_model import KINDS


class SpatialPositionHead(nn.Module):
    def __init__(self, state_width=512, grid=16, key_width=64, spatial_channels=3):
        super().__init__()
        if state_width < 1 or grid < 2 or key_width < 1 or spatial_channels < 1:
            raise ValueError("invalid spatial position head dimensions")
        self.grid = grid
        self.key_width = key_width
        self.spatial_channels = spatial_channels
        self.kind = nn.Embedding(len(KINDS), 32)
        self.cells = nn.Sequential(nn.Conv2d(spatial_channels + 2, key_width, 3, padding=1), nn.GELU(),
                                   nn.Conv2d(key_width, key_width, 3, padding=1), nn.GELU())
        self.query = nn.Sequential(nn.Linear(state_width + 34, key_width),
                                   nn.LayerNorm(key_width), nn.GELU())
        self.offset = nn.Sequential(nn.Linear(key_width * 2 + 2, key_width), nn.GELU(),
                                    nn.Linear(key_width, 2))

    def forward(self, state, spatial, actor_xy, kind):
        batch = state.shape[0]
        if (state.ndim != 2 or spatial.ndim != 4 or spatial.shape[0] != batch or
                spatial.shape[1] != self.spatial_channels or actor_xy.shape != (batch, 2) or
                kind.shape != (batch,)):
            raise ValueError("misaligned spatial position inputs")
        pooled = F.adaptive_avg_pool2d(spatial, (self.grid, self.grid))
        axis = (torch.arange(self.grid, device=spatial.device,
                             dtype=spatial.dtype) + 0.5) / self.grid
        y, x = torch.meshgrid(axis, axis, indexing="ij")
        coords = torch.stack((x, y))[None].expand(batch, -1, -1, -1)
        keys = self.cells(torch.cat((pooled, coords), dim=1))
        query = self.query(torch.cat((state, self.kind(kind), actor_xy), dim=-1))
        logits = (keys * query[:, :, None, None]).sum(dim=1) / math.sqrt(self.key_width)
        return dict(logits=logits.flatten(1), keys=keys, query=query, actor_xy=actor_xy)

    def offsets(self, output, cell_index):
        batch = output["query"].shape[0]
        if cell_index.shape != (batch,):
            raise ValueError("one cell index required per example")
        cells = output["keys"].flatten(2).transpose(1, 2)
        chosen = cells[torch.arange(batch, device=cell_index.device), cell_index]
        return self.offset(torch.cat((output["query"], chosen, output["actor_xy"]), dim=-1))

    def loss(self, output, target_xy):
        if target_xy.shape != (output["logits"].shape[0], 2):
            raise ValueError("one normalized position required per example")
        scaled = target_xy.clamp(0, 1 - 1e-7) * self.grid
        cell = scaled.long()
        index = cell[:, 1] * self.grid + cell[:, 0]
        fraction = scaled - cell
        return (F.cross_entropy(output["logits"], index) +
                F.smooth_l1_loss(self.offsets(output, index).sigmoid(), fraction))

    def decode(self, output):
        index = output["logits"].argmax(dim=1)
        fraction = self.offsets(output, index).sigmoid()
        cell = torch.stack((index % self.grid, index // self.grid), dim=-1)
        return (cell + fraction) / self.grid


def entity_raster(batch, grid):
    """Player-visible own, visible enemy, remembered enemy and neutral density."""
    if grid < 2:
        raise ValueError("invalid spatial grid")
    xy = batch["entity_numeric"][..., :2]
    relation = batch["relation"]
    mask = batch["entity_mask"]
    visible = batch["entity_numeric"][..., 4] >= 0.5
    if xy.ndim != 3 or xy.shape[-1] != 2 or relation.shape != xy.shape[:2] or mask.shape != relation.shape:
        raise ValueError("misaligned entity raster inputs")
    cell = (xy.clamp(0, 1 - 1e-7) * grid).long()
    index = cell[..., 1] * grid + cell[..., 0]
    layers = (mask & (relation == 0), mask & (relation == 1) & visible,
              mask & (relation == 1) & ~visible, mask & (relation == 2))
    result = xy.new_zeros((xy.shape[0], 4, grid * grid))
    for channel, active in enumerate(layers):
        result[:, channel].scatter_add_(1, index, active.to(xy.dtype))
    return result.reshape(xy.shape[0], 4, grid, grid).log1p()
