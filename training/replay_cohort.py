"""Freeze conservative ladder quality, duplicate groups, and untouched holdouts."""
from datetime import datetime
import hashlib
import json


def stable(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, ensure_ascii=True).encode()).hexdigest()


def player_identity(player):
    ids = {c.get("aurora_id") for c in player.get("source_claims", []) if c.get("aurora_id") is not None}
    if len(ids) == 1:
        return "aurora:" + str(next(iter(ids)))
    return "name:" + player["name"].casefold()


def qualified(player):
    # Only an exact player-matched source claim on this replay qualifies it.
    # Unverified 'pro' tags and the opponent's MMR never grant quality.
    return player["race"] == "P" and any(
        type(c.get("mmr_claim")) in (int, float) and c["mmr_claim"] >= 2000
        for c in player.get("source_claims", []))


def freeze(records):
    groups = {}
    aliases = {}
    for record in sorted(records, key=lambda r: r["path"]):
        players = sorted(record["players"], key=lambda p: p["slot_id"])
        if (record.get("parse_status") != "parsed" or record.get("quarantine_reasons")
                or len(players) != 2 or record["frames"] < 3 * 60 * 24
                or not any(qualified(p) for p in players)):
            continue
        identities = [player_identity(p) for p in players]
        timestamp = datetime.fromisoformat(record["start_time"]).timestamp()
        # Alternate recordings stay together even when their byte hashes differ.
        group = stable([record["map_sha256"], timestamp,
                        sorted((p["name"].casefold(), p["race"]) for p in players)])
        if group in groups:
            aliases[record["sha256"]] = group
            continue
        gid = "game:" + group
        groups[group] = {"game_id": gid, "duplicate_group": group,
            "replay_sha256": record["sha256"], "path": record["path"],
            "races": [p["race"] for p in players], "slots": [p["slot_id"] for p in players],
            "player_quality": ["qualified_ladder" if qualified(p) else "unknown" for p in players],
            "player_keys": [stable(p) for p in identities],
            "map_name": record["map_name"], "timestamp": timestamp,
            "valid_through_frame": record["frames"],
            "player_holdout": any(int(stable(identity)[:8], 16) % 100 < 5 for identity in identities),
            "map_holdout": "Radeon" in "".join(c for c in record["map_name"] if ord(c) >= 32)}
    games = sorted(groups.values(), key=lambda g: (g["timestamp"], g["game_id"]))
    if len(games) < 10:
        raise ValueError("Too few qualified games for chronological splits")
    # A single timestamp cannot straddle a boundary.
    train_before = games[int(len(games) * .8)]["timestamp"]
    test_from = games[int(len(games) * .9)]["timestamp"]
    for game in games:
        if game["player_holdout"] or game["map_holdout"] or game["timestamp"] >= test_from:
            game["split"] = "test"
        elif game["timestamp"] >= train_before:
            game["split"] = "validation"
        else:
            game["split"] = "train"
    return {"version": 1, "quality_rule": "exact-player source MMR >= 2000; pro tags unverified",
            "train_before": train_before, "test_from": test_from,
            "test_holdouts": "latest 10% by time; Radeon; stable 5% player IDs (either side)",
            "duplicate_aliases": aliases, "games": games}
