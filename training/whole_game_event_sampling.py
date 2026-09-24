"""Natural event-class schedule with equal matchup exposure for GPU fitting."""
from __future__ import annotations

from collections import Counter
import hashlib
import json
from pathlib import Path
import random

from .whole_game_event_prior import count_game
from .whole_game_fit import MATCHUPS
from .whole_game_release import key


def group_event_counts(release, group):
    release = Path(release)
    identity = json.loads((release / "identity.json").read_text(encoding="utf8"))
    records = {record["game_id"]: record for record in identity["selected"]
               if record["split"] == "train"}
    counts = {matchup: Counter() for matchup in MATCHUPS}
    for matchup in MATCHUPS:
        for game_id in group[matchup]:
            record = records.get(game_id)
            if record is None or record["matchup"] != matchup:
                raise ValueError("event schedule cohort changed")
            directory = release / "games" / key(record)
            if not (directory / "receipt.json").exists():
                raise ValueError("event schedule lacks verified replay receipt")
            windows, events = count_game(directory)
            counts[matchup].update(windows=windows, events=events)
    if any(not counts[matchup]["windows"] for matchup in MATCHUPS):
        raise ValueError("event schedule lacks complete windows")
    return counts


def natural_event_schedule(categories, counts, slots, *, seed):
    categories = sorted(categories)
    if slots < 1 or not categories:
        raise ValueError("event schedule needs classes and slots")
    weights = {}
    for category in categories:
        if len(category) != 3 or category[0] not in MATCHUPS or category[1] != "event" or \
                category[2] not in ("0", "1"):
            raise ValueError("unexpected event category")
        matchup = category[0]
        windows, events = counts[matchup]["windows"], counts[matchup]["events"]
        if not 0 <= events <= windows or windows == 0:
            raise ValueError("invalid training event counts")
        fraction = (events if category[2] == "1" else windows - events) / windows
        weights[category] = fraction / len(MATCHUPS)
    total = sum(weights.values())
    if total <= 0:
        raise ValueError("no event category has training mass")
    quota = {category: slots * weights[category] / total for category in categories}
    allocated = {category: int(quota[category]) for category in categories}
    remaining = slots - sum(allocated.values())
    order = sorted(categories, key=lambda category: (-(quota[category] - allocated[category]),
                                                     category))
    for category in order[:remaining]:
        allocated[category] += 1
    schedule = [category for category in categories for _ in range(allocated[category])]
    random.Random(seed).shuffle(schedule)
    if len(schedule) != slots:
        raise AssertionError("event schedule length changed")
    return schedule


def group_seed(group):
    token = hashlib.sha256(json.dumps(group, sort_keys=True).encode()).digest()
    return int.from_bytes(token[:8], "little")
