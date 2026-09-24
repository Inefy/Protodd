"""Player-observable future-state targets for whole-game auxiliary learning.

Targets are derived strictly from later legal observations and confirmed own
commands. They are never added to the input row or recurrent policy state.
"""
from __future__ import annotations

import argparse
from collections import Counter, deque
import json
from pathlib import Path

from .whole_game_model import DOMAINS
from .whole_game_shards import selected_shards, trajectory_shard

SCHEMA = "protodd-whole-game-future-v1"
HORIZONS = (24, 240, 1440)


def derive(now, later, confirmed_domains):
    if (now["reason"] != "cadence" or later["reason"] != "cadence"
            or now["perspective"] != later["perspective"]
            or now["frame"] >= later["frame"]
            or len(now["vision"]) != len(later["vision"])):
        raise ValueError("future target needs later legal cadence from same perspective/map")
    if set(now["vision"]) - set("012") or set(later["vision"]) - set("012"):
        raise ValueError("invalid legal vision state")
    if any(before != "0" and after == "0" for before, after in zip(now["vision"], later["vision"])):
        raise ValueError("explored map regressed")
    own_now = {e["id"]: e for e in now["entities"] if e["relation"] == 0}
    own_later = {e["id"]: e for e in later["entities"] if e["relation"] == 0}
    enemy_now = {e["id"] for e in now["entities"] if e["relation"] == 1}
    enemy_later = {e["id"] for e in later["entities"] if e["relation"] == 1}
    common = own_now.keys() & own_later.keys()
    tech_gain = sum(b > a for a, b in zip(now["technology_completed"], later["technology_completed"]))
    upgrade_gain = sum(b > a for a, b in zip(now["upgrade_levels"], later["upgrade_levels"]))
    known_domains = set(confirmed_domains)
    if known_domains - set(DOMAINS):
        raise ValueError("unknown confirmed command domain")
    return dict(schema=SCHEMA, perspective=now["perspective"], source_frame=now["frame"],
                target_frame=later["frame"], horizon=later["frame"] - now["frame"],
                newly_explored_tiles=sum(a == "0" and b != "0" for a, b in zip(now["vision"], later["vision"])),
                newly_known_enemy_ids_retained=len(enemy_later - enemy_now),
                visible_enemies_at_target=sum(e["relation"] == 1 and e["visible"] for e in later["entities"]),
                own_entities_missing=len(own_now.keys() - own_later.keys()),
                own_entities_added=len(own_later.keys() - own_now.keys()),
                own_hp_shield_loss_common=sum(max(0, own_now[i]["hp"] + own_now[i]["shields"]
                    - own_later[i]["hp"] - own_later[i]["shields"]) for i in common),
                minerals_delta=later["minerals"] - now["minerals"], gas_delta=later["gas"] - now["gas"],
                supply_used_delta=later["supply_used"] - now["supply_used"],
                technology_completed_gain=tech_gain, upgrade_level_gain=upgrade_gain,
                confirmed_command_domains=[int(domain in known_domains) for domain in DOMAINS])


def future_targets(trajectory, horizons=HORIZONS):
    horizons = tuple(sorted(set(horizons)))
    if not horizons or any(type(h) is not int or h < 24 or h % 24 for h in horizons):
        raise ValueError("positive 24-frame-multiple horizons required")
    pending = {}
    confirmed = deque()
    max_horizon = max(horizons)
    last_frame = -1
    for row, supervision in trajectory:
        frame = row["frame"]
        if frame < last_frame:
            raise ValueError("noncausal trajectory")
        last_frame = frame
        if supervision["action"] is not None:
            confirmed.append((frame, supervision["action"]["domain"]))
        if not supervision["update_memory"]:
            continue
        while confirmed and confirmed[0][0] < frame - max_horizon:
            confirmed.popleft()
        for horizon in horizons:
            source = pending.get(frame - horizon)
            if source is not None:
                domains = {domain for when, domain in confirmed if source["frame"] <= when < frame}
                yield source, derive(source, row, domains)
        pending[frame] = row
        pending.pop(frame - max_horizon - 24, None)


def audit(release, split="train", max_games=3, horizons=HORIZONS):
    counts, per_horizon = Counter(), {str(h): Counter() for h in horizons}
    for record, directory, _ in selected_shards(Path(release), split):
        if counts["games"] >= max_games:
            break
        counts["games"] += 1
        for source, target in future_targets(trajectory_shard(directory), horizons):
            stats = per_horizon[str(target["horizon"])]
            stats["pairs"] += 1
            stats["exploration_positive"] += target["newly_explored_tiles"] > 0
            stats["new_enemy_positive"] += target["newly_known_enemy_ids_retained"] > 0
            stats["own_damage_positive"] += target["own_hp_shield_loss_common"] > 0
            stats["tech_positive"] += target["technology_completed_gain"] + target["upgrade_level_gain"] > 0
            for domain, present in zip(DOMAINS, target["confirmed_command_domains"]):
                stats[f"confirmed_{domain}"] += present
    return dict(schema=SCHEMA, split=split, games=counts["games"], horizons=per_horizon,
                limitation="targets reflect later legal observations; future commands are labels only, not inputs")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("release", type=Path)
    parser.add_argument("--split", choices=("train", "validation"), default="train")
    parser.add_argument("--max-games", type=int, default=3)
    args = parser.parse_args()
    print(json.dumps(audit(args.release, args.split, args.max_games), indent=2, default=dict))


if __name__ == "__main__":
    main()
