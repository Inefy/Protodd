"""Blend empirical replay command frequency with rare-action exposure."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import random

from .whole_game_release import key


def group_kind_counts(release, group):
    """Count labels from the exact replay group after its shards were verified."""
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    records = {record["game_id"]: record for record in identity["selected"]
               if record["split"] == "train"}
    counts = Counter()
    for matchup, ids in group.items():
        for game_id in ids:
            record = records.get(game_id)
            if record is None or record["matchup"] != matchup:
                raise ValueError("sampling cohort changed")
            receipt = json.loads((release / "games" / key(record) / "receipt.json").read_text())
            for kind, count in receipt["report"]["labels"]["kinds"].items():
                counts[(matchup, kind)] += count
    if not counts:
        raise ValueError("no command frequencies in replay group")
    return counts


def mixed_action_schedule(categories, kind_counts, slots, *, natural_share=0.5, seed=42):
    """Deterministic quota schedule; rare and frequent kinds both receive updates."""
    categories = sorted(categories)
    if not categories or slots < 1 or not 0 <= natural_share <= 1:
        raise ValueError("invalid action schedule")
    same_kind = Counter((category[0], category[2]) for category in categories)
    empirical = {category: kind_counts.get((category[0], category[2]), 0) /
                 same_kind[(category[0], category[2])] for category in categories}
    total = sum(empirical.values())
    if total <= 0:
        raise ValueError("selected categories have no replay commands")
    weights = {category: (1 - natural_share) / len(categories) +
               natural_share * empirical[category] / total for category in categories}
    quota = {category: slots * weight for category, weight in weights.items()}
    allocated = {category: int(quota[category]) for category in categories}
    remaining = slots - sum(allocated.values())
    remainders = sorted(categories, key=lambda category: (-(quota[category] - allocated[category]),
                                                         category))
    for category in remainders[:remaining]:
        allocated[category] += 1
    schedule = [category for category in categories for _ in range(allocated[category])]
    random.Random(seed).shuffle(schedule)
    if len(schedule) != slots:
        raise AssertionError("action schedule size changed")
    return schedule


def group_seed(group):
    encoded = json.dumps(group, sort_keys=True, separators=(",", ":")).encode()
    return int.from_bytes(hashlib.sha256(encoded).digest()[:8], "little")
