"""Legal v3.2 observation tensors and partial command targets.

Entity selection is deterministic from observations alone; labels never choose
which units enter the model. Overflow is an input feature and reported to the
caller. Target supervision is masked when a legal target cannot be represented.
"""
from __future__ import annotations

import math

import numpy as np
import torch

from .whole_game_model import DOMAINS, ENTITY_NUMERIC, KINDS, TARGET_MODES
from .whole_game_pilot import SCHEMA


def static_grid(terrain):
    if terrain["schema"] != "protodd-terrain-v2":
        raise ValueError("unsupported terrain")
    height, width = terrain["height_walktiles"], terrain["width_walktiles"]
    if height % 4 or width % 4 or len(terrain["walkability"]) != width * height:
        raise ValueError("invalid terrain dimensions")
    walk = np.frombuffer(terrain["walkability"].encode("ascii"), dtype=np.uint8) - 48
    walk = walk.reshape(height // 4, 4, width // 4, 4).mean(axis=(1, 3))
    return torch.from_numpy(walk.astype("float32"))


def select_entities(row, limit):
    if limit < 1:
        raise ValueError("positive entity limit required")
    entities = row["entities"]
    own = [e for e in entities if e["relation"] == 0]
    if len(own) > limit:
        raise ValueError("all own units must fit; increase entity capacity")
    origin = (sum(e["position"][0] for e in own) / max(1, len(own)),
              sum(e["position"][1] for e in own) / max(1, len(own)))
    def distance(e):
        return (e["position"][0] - origin[0]) ** 2 + (e["position"][1] - origin[1]) ** 2
    own.sort(key=lambda e: e["id"])
    visible = sorted((e for e in entities if e["relation"] == 1 and e["visible"]),
                     key=lambda e: (distance(e), e["id"]))
    memory = sorted((e for e in entities if e["relation"] == 1 and not e["visible"]),
                    key=lambda e: (-e["last_seen"], distance(e), e["id"]))
    neutral = sorted((e for e in entities if e["relation"] == 2),
                     key=lambda e: (distance(e), e["id"]))
    selected = (own + visible + memory + neutral)[:limit]
    return selected, len(entities) - len(selected)


def encode_observation(row, terrain_grid, limit=512):
    if row["schema"] != SCHEMA or row["reason"] not in ("cadence", "before_command"):
        raise ValueError("incompatible observation")
    height, width = terrain_grid.shape
    if len(row["vision"]) != width * height or len(row["technology_completed"]) != 44 or len(row["upgrade_levels"]) != 61:
        raise ValueError("incomplete map or technology state")
    selected, overflow = select_entities(row, limit)
    if not selected:
        raise ValueError("empty legal entity set")
    ids = {entity["id"]: index for index, entity in enumerate(selected)}
    types, relations, orders, numeric = [], [], [], []
    for e in selected:
        own = e["own_state"] if e["relation"] == 0 else None
        order_position = own["order_position"] if own else [-1, -1]
        values = [e["position"][0] / (width * 32), e["position"][1] / (height * 32),
                  math.log1p(max(0, e["hp"])) / 12, math.log1p(max(0, e["shields"])) / 12,
                  float(bool(e["visible"])), float(bool(e["completed"])),
                  min(1, (row["frame"] - e["last_seen"]) / 3000),
                  (own["energy"] / 250) if own else 0,
                  (own["ground_cooldown"] / 60) if own else 0,
                  (own["air_cooldown"] / 60) if own else 0,
                  order_position[0] / (width * 32) if order_position[0] >= 0 else 0,
                  order_position[1] / (height * 32) if order_position[1] >= 0 else 0,
                  float(bool(own["loaded"])) if own else 0,
                  min(1, len(own["queue"]) / 5) if own else 0,
                  min(1, len(own["cargo"]) / 8) if own else 0,
                  e["first_seen"] / 100000]
        if len(values) != ENTITY_NUMERIC:
            raise AssertionError("entity feature width changed")
        numeric.append(values)
        types.append(e["type"])
        relations.append(e["relation"])
        orders.append((own["order"] + 1) if own else 0)
    vision = np.frombuffer(row["vision"].encode("ascii"), dtype=np.uint8).reshape(height, width)
    spatial = torch.stack((torch.from_numpy((vision >= 49).astype("float32")),
                           torch.from_numpy((vision == 50).astype("float32")), terrain_grid))
    global_values = [math.log1p(row["minerals"]) / 10, math.log1p(row["gas"]) / 10,
                     row["supply_used"] / 400, row["supply_total"] / 400,
                     row["frame"] / 100000, overflow / limit]
    for name in ("technology_completed", "technology_in_progress", "upgrade_levels", "upgrade_in_progress"):
        global_values.extend(row[name])
    batch = dict(type=torch.tensor(types, dtype=torch.long)[None],
                 relation=torch.tensor(relations, dtype=torch.long)[None],
                 order=torch.tensor(orders, dtype=torch.long)[None],
                 entity_numeric=torch.tensor(numeric, dtype=torch.float32)[None],
                 entity_mask=torch.ones((1, len(selected)), dtype=torch.bool),
                 spatial=spatial[None])
    batch["global"] = torch.tensor(global_values, dtype=torch.float32)[None]
    return batch, ids, overflow


def encode_label(label, ids, width, height):
    actions, masks = label["actions"], label["loss_masks"].copy()
    if label["schema"] != "protodd-imitation-label-v1":
        raise ValueError("unsupported label")
    target = dict(domain=torch.tensor([DOMAINS.index(label["domain"])]),
                  kind=torch.tensor([KINDS.index(actions["kind"])]),
                  queued=torch.tensor([int(actions["queued"])]),
                  mask=dict(masks, domain=True))
    if masks["target_mode"]:
        target["target_mode"] = torch.tensor([TARGET_MODES.index(actions["target_mode"])])
    for name in ("order", "unit_type", "technology", "upgrade", "queue_slot"):
        if masks[name]:
            index = actions[name]
            bound = {"order": 256, "unit_type": 256, "technology": 44, "upgrade": 61, "queue_slot": 16}[name]
            if not 0 <= index < bound:
                target["mask"][name] = False
            else:
                target[name] = torch.tensor([index])
    positives, negatives = set(label["actor_positive"]), set(label["actor_negative"])
    if not positives <= ids.keys():
        raise ValueError("own confirmed actor was silently truncated")
    target["actor_known"] = torch.tensor([[entity in positives or entity in negatives for entity in ids]], dtype=torch.bool)
    target["actor_positive"] = torch.tensor([[entity in positives for entity in ids]], dtype=torch.bool)
    if masks["target_entity"]:
        entity = actions["target_entity"]
        if entity in ids:
            target["target_entity"] = torch.tensor([ids[entity]])
        else:
            target["mask"]["target_entity"] = False
    if masks["target_position"]:
        x, y = actions["target_position"]
        factor = 32 if label["coordinate_space"] == "build_tile" else 1
        target["target_position"] = torch.tensor([[x * factor / (width * 32),
                                                    y * factor / (height * 32)]], dtype=torch.float32)
    return target
