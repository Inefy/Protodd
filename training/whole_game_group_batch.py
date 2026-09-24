"""Group-command supervised loss, discrete map targets and actor cardinality."""
from __future__ import annotations

import torch
from torch.nn import functional as F

from .whole_game_batch import collate_observations
from .whole_game_cadence_sequences import slot_targets
from .whole_game_features import encode_label
from .whole_game_action_schema import supported_pair_mask
from .whole_game_model import KINDS, TARGET_MODES, masked_action_loss
from .whole_game_group_model import position_classes
from .whole_game_structured_loss import balanced_actor_loss


def conditioned_action_loss(output, label):
    """Supervise observed arguments, group membership and categorical positions."""
    label = dict(label, mask=dict(label["mask"]))
    has_position = label["mask"].pop("target_position", False)
    loss, _ = masked_action_loss(output, label)
    if label["mask"].get("actors", False):
        valid = label["actor_known"] & torch.isfinite(output["actor"])
        if valid.any():
            original = F.binary_cross_entropy_with_logits(
                output["actor"][valid], label["actor_positive"][valid].float())
            loss = loss - original + balanced_actor_loss(
                output["actor"], label["actor_known"], label["actor_positive"])
    if label["mask"].get("kind", False) and label["mask"].get("target_mode", False):
        allowed = supported_pair_mask(output["kind"].device)[label["kind"]]
        if allowed.gather(1, label["target_mode"][:, None]).all():
            modes = output["target_mode"]
            loss = loss + (torch.logsumexp(modes, dim=-1) -
                           torch.logsumexp(modes.masked_fill(~allowed, -torch.inf),
                                           dim=-1)).mean()
    if has_position:
        cell, sub = position_classes(label["target_position"])
        loss = loss + F.cross_entropy(output["position_cell"], cell) + F.cross_entropy(output["position_subcell"], sub)
    if label["mask"].get("actors", False):
        count = label["actor_positive"].sum(-1) - 1
        if (count < 0).any() or (count >= output["actor_count"].shape[-1]).any():
            raise ValueError("unsupported actor-set size")
        loss = loss + F.cross_entropy(output["actor_count"], count)
    return loss


def multislot_minibatch_loss(model, samples, device, cache=None, *, report=True):
    """Samples are (prior cadence rows, current row, sequence, terrain).

    Future commands enter only as decoder teacher tokens and supervised targets.
    The encoder and persistent game memory see current and earlier observations.
    """
    if not samples or len({tuple(sample[3].shape) for sample in samples}) != 1:
        raise ValueError("nonempty minibatch with matching map dimensions required")
    if any(sample[2]["observation"] is not sample[1] for sample in samples):
        raise ValueError("sequence must be bound to its causal cadence row")
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
    batch, ids, _ = collate_observations([sample[1] for sample in samples],
                                         terrains, device, cache)
    targets = [slot_targets(sample[2], maximum_commands=model.maximum_slots)
               for sample in samples]
    steps = max(len(target["slots"]) for target in targets)
    own_indices = []
    for sample, mapping in zip(samples, ids):
        own_ids = {entity["id"] for entity in sample[1]["entities"]
                   if entity["relation"] == 0}
        own_indices.append(next((index for token, index in mapping.items()
                                 if token in own_ids), None))
    if any(index is None for index in own_indices):
        raise ValueError("command sequence has no encoded owned actor")
    teachers = []
    for step in range(steps):
        fields = {name: [] for name in ("actor", "kind", "target_mode", "delay",
                                      "target_entity", "position", "has_position",
                                      "unit_type")}
        for sample_index, target in enumerate(targets):
            slot = target["slots"][step] if step < len(target["slots"]) else None
            label = None if slot is None else slot["action"]
            actor = own_indices[sample_index]
            if label is not None and slot["actor_available"]:
                candidates = sorted(set(label["actor_positive"]) & ids[sample_index].keys())
                if candidates:
                    actor = ids[sample_index][candidates[0]]
            fields["actor"].append(actor)
            fields["kind"].append(KINDS.index(label["actions"]["kind"])
                                  if label is not None else 0)
            fields["target_mode"].append(TARGET_MODES.index(
                label["actions"]["target_mode"]) if label is not None and
                label["loss_masks"]["target_mode"] else 0)
            fields["delay"].append(slot["delay_frames"] if label is not None else 0)
            actions = label["actions"] if label is not None else {}
            masks = label["loss_masks"] if label is not None else {}
            target_entity = actions.get("target_entity")
            fields["target_entity"].append(
                ids[sample_index].get(target_entity, -1) if slot is not None and
                slot["target_available"] and masks.get("target_entity") else -1)
            has_position = bool(masks.get("target_position"))
            fields["has_position"].append(has_position)
            if has_position:
                factor = 32 if label["coordinate_space"] == "build_tile" else 1
                x, y = actions["target_position"]
                fields["position"].append((x * factor / (sample[3].shape[1] * 32),
                                           y * factor / (sample[3].shape[0] * 32)))
            else:
                fields["position"].append((0.0, 0.0))
            unit_type = actions.get("unit_type") if masks.get("unit_type") else None
            fields["unit_type"].append(unit_type if isinstance(unit_type, int) and
                                       0 <= unit_type < 256 else 0)
        actor_sets = []
        for sample_index, target in enumerate(targets):
            slot = target["slots"][step] if step < len(target["slots"]) else None
            label = None if slot is None else slot["action"]
            selected = torch.zeros(batch["type"].shape[1], dtype=torch.bool, device=device)
            positives = set(label["actor_positive"]) if label is not None and slot["actor_available"] else set()
            for token, entity_index in ids[sample_index].items():
                if token in positives:
                    selected[entity_index] = True
            if not selected.any():
                selected[own_indices[sample_index]] = True
            actor_sets.append(selected)
        teachers.append({name: torch.tensor(values, dtype=(
            torch.float32 if name == "position" else torch.bool
            if name == "has_position" else torch.long), device=device)
                         for name, values in fields.items()})
        teachers[-1]["actor_set"] = torch.stack(actor_sets)
    decoded = model.forward_slots(batch, memory, teacher_tokens=teachers,
                                  slots=steps)["slots"]
    stops, delays, actions = [], [], []
    for step, output in enumerate(decoded):
        for index, (sample, target) in enumerate(zip(samples, targets)):
            if step >= len(target["slots"]):
                continue
            slot = target["slots"][step]
            stops.append(F.cross_entropy(output["stop"][index:index + 1],
                                         torch.tensor([int(slot["stop"])], device=device)))
            if slot["stop"]:
                continue
            delays.append(F.cross_entropy(output["delay"][index:index + 1],
                                          torch.tensor([slot["delay_frames"]], device=device)))
            if not slot["actor_available"]:
                continue
            terrain = sample[3]
            encoded = encode_label(slot["action"], ids[index],
                                   terrain.shape[1], terrain.shape[0])
            if not slot["target_available"]:
                encoded["mask"]["target_entity"] = False
            for name in ("actor_known", "actor_positive"):
                encoded[name] = F.pad(encoded[name],
                                      (0, output["actor"].shape[1] - encoded[name].shape[1]))
            encoded = {name: value.to(device) if isinstance(value, torch.Tensor) else value
                       for name, value in encoded.items()}
            single = {name: value[index:index + 1] for name, value in output.items()}
            actions.append(conditioned_action_loss(single, encoded))
    if not stops:
        raise ValueError("no supervised STOP or action slots")
    stop_loss = torch.stack(stops).mean()
    action_loss = torch.stack(actions).mean() if actions else stop_loss.new_zeros(())
    delay_loss = torch.stack(delays).mean() if delays else stop_loss.new_zeros(())
    total = action_loss + stop_loss + 0.25 * delay_loss
    if not report:
        return total, None
    return total, dict(stop=float(stop_loss.detach()), action=float(action_loss.detach()),
                       delay=float(delay_loss.detach()), supervised_actions=len(actions),
                       overflow_commands=sum(target["overflow_commands"] for target in targets))
