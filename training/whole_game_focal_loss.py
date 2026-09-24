"""Action focal objective for cadence-trained, actor-conditioned teachers.

The structured objective already balances positive/negative actor evidence and
penalizes unsupported kind/mode pairs. Replace only its categorical kind term
so easy majority commands stop dominating updates after they are learned.
"""
from __future__ import annotations

import torch
from torch.nn import functional as F

from .whole_game_structured_loss import structured_action_loss


GAMMA = 2.0


def focal_structured_action_loss(output, label):
    loss, diagnostics = structured_action_loss(output, label)
    if not label["mask"].get("kind", False):
        return loss, diagnostics
    logits = output["kind"]
    target = label["kind"]
    cross_entropy = F.cross_entropy(logits, target)
    probability = torch.exp(-cross_entropy)
    focal = (1.0 - probability).pow(GAMMA) * cross_entropy
    diagnostics = dict(diagnostics, kind=focal.detach())
    return loss - cross_entropy + focal, diagnostics
