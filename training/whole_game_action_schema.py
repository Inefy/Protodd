"""Action/target pairs executable by the Protoss whole-game runtime."""
from __future__ import annotations

import torch

from .whole_game_model import KINDS, TARGET_MODES


KIND_TARGET_MODES = {
    **{kind: {"position"} for kind in
       ("move", "attack_move", "patrol", "unload_position", "build")},
    **{kind: {"entity"} for kind in
       ("follow", "gather", "load", "unload_unit", "merge_archon", "merge_dark_archon")},
    **{kind: {"none"} for kind in
       ("train", "research", "upgrade", "cancel_queue", "cancel_build",
        "cancel_research", "cancel_upgrade", "stop", "return_cargo",
        "train_fighter", "hold")},
    **{kind: {"entity", "position"} for kind in ("right_click", "attack", "rally")},
    "cast": set(TARGET_MODES), "unload_all": {"none", "position"},
}


def supported_pair_mask(device=None):
    return torch.tensor([[mode in KIND_TARGET_MODES.get(kind, ()) for mode in TARGET_MODES]
                         for kind in KINDS], dtype=torch.bool, device=device)


def select_supported_pair(kind_logits, mode_logits):
    """Project independent heads onto Protoss commands the runtime can issue."""
    if kind_logits.ndim != 1 or kind_logits.numel() != len(KINDS):
        raise ValueError("invalid kind head")
    if mode_logits.ndim != 1 or mode_logits.numel() != len(TARGET_MODES):
        raise ValueError("invalid target-mode head")
    if not torch.isfinite(kind_logits).all() or not torch.isfinite(mode_logits).all():
        raise ValueError("nonfinite action head")
    scores = kind_logits[:, None] + mode_logits[None, :]
    index = scores.masked_fill(~supported_pair_mask(scores.device), -torch.inf).flatten().argmax().item()
    return KINDS[index // len(TARGET_MODES)], TARGET_MODES[index % len(TARGET_MODES)]
