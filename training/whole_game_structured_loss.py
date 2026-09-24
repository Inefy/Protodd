"""Experimental action objective that preserves positive actor supervision.

The baseline averages actor BCE over every known unit. In a large army, one
confirmed actor is overwhelmed by dozens of negative units. This objective
gives positive and negative groups equal total weight for each command.
"""
from __future__ import annotations

import torch
from torch.nn import functional as F

from .whole_game_action_schema import supported_pair_mask
from .whole_game_model import masked_action_loss


def balanced_actor_loss(logits, known, positive):
    if logits.shape != known.shape or logits.shape != positive.shape:
        raise ValueError("actor logits and masks must align")
    valid = known & torch.isfinite(logits)
    confirmed = valid & positive
    if not confirmed.any():
        raise ValueError("confirmed actor required for balanced loss")
    rejected = valid & ~positive
    result = F.softplus(-logits[confirmed]).mean()
    if rejected.any():
        result = result + F.softplus(logits[rejected]).mean()
    return result


def target_compatibility_loss(kind_logits, mode_logits):
    """Negative log mass assigned to executable kind/target-mode pairs."""
    if kind_logits.ndim != 2 or mode_logits.ndim != 2 or \
            kind_logits.shape[0] != mode_logits.shape[0]:
        raise ValueError("batched kind and target-mode logits required")
    legal = supported_pair_mask(kind_logits.device)
    if kind_logits.shape[1] != legal.shape[0] or mode_logits.shape[1] != legal.shape[1]:
        raise ValueError("action schema and logits do not align")
    scores = kind_logits[:, :, None] + mode_logits[:, None, :]
    all_pairs = torch.logsumexp(scores.flatten(1), dim=1)
    executable = torch.logsumexp(scores.masked_fill(~legal, -torch.inf).flatten(1), dim=1)
    return (all_pairs - executable).mean()


def structured_action_loss(output, label):
    """Keep all masked heads; replace only the actor imbalance in the baseline."""
    loss, diagnostics = masked_action_loss(output, label)
    diagnostics = dict(diagnostics)
    if label["mask"].get("actors", False):
        valid = label["actor_known"] & torch.isfinite(output["actor"])
        if valid.any():
            old = F.binary_cross_entropy_with_logits(
                output["actor"][valid], label["actor_positive"][valid].float())
            actor = balanced_actor_loss(output["actor"], label["actor_known"],
                                        label["actor_positive"])
            loss = loss - old + actor
            diagnostics["actors"] = actor.detach()
    if label["mask"].get("kind", False) and label["mask"].get("target_mode", False):
        legal = supported_pair_mask(output["kind"].device)
        if legal[label["kind"], label["target_mode"]].all():
            compatibility = target_compatibility_loss(output["kind"], output["target_mode"])
            loss = loss + compatibility
            diagnostics["compatibility"] = compatibility.detach()
    return loss, diagnostics
