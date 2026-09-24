"""Bind exact-player ladder MMR claims to a frozen whole-game release.

Catalog MMR is a source claim, not a verified professional identity or an
outcome label. This sidecar never changes replay extraction or split assignment.
"""
from __future__ import annotations

import argparse
from collections import Counter, defaultdict
from contextlib import closing
import hashlib
import json
from pathlib import Path
import sqlite3


SCHEMA = "protodd-whole-game-quality-v2"


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def actor_group(player):
    """Private stable grouping for diversity; zero Aurora IDs are not identities."""
    claims = player.get("source_claims", [])
    ids = {claim.get("aurora_id") for claim in claims
           if type(claim.get("aurora_id")) is int and claim["aurora_id"] > 0}
    identity = ("aurora:" + str(next(iter(ids))) if len(ids) == 1 else
                "name:" + player["name"].casefold())
    return hashlib.sha256(identity.encode("utf8")).hexdigest()


def build_index(identity_path, audit_path):
    identity_path, audit_path = Path(identity_path), Path(audit_path)
    identity = json.loads(identity_path.read_text(encoding="utf8"))
    # The release selects only training and validation perspectives. A test
    # perspective must never enter this sampling index.
    selected = identity["selected"]
    if any(row["split"] not in ("train", "validation") for row in selected):
        raise ValueError("quality index cannot include the sealed test split")
    games = {}
    counts = defaultdict(lambda: defaultdict(Counter))
    with closing(sqlite3.connect(f"file:{audit_path.resolve().as_posix()}?mode=ro", uri=True)) as connection:
        for item in selected:
            result = connection.execute("SELECT sha256,status,detail FROM replays WHERE path=?",
                                        (item["path"],)).fetchone()
            if result is None or result[0] != item["replay_sha256"] or result[1] != "parsed":
                raise ValueError(f"missing or mismatched replay audit: {item['game_id']}")
            detail = json.loads(result[2])
            players = sorted(detail["players"], key=lambda p: p["slot_id"])
            perspective = item["perspective"]
            if perspective >= len(players) or players[perspective]["race"] != "P":
                raise ValueError(f"invalid Protoss perspective: {item['game_id']}")
            claims = players[perspective].get("source_claims", [])
            ratings = [c["mmr_claim"] for c in claims
                       if c.get("catalog_side") in ("primary", "opponent")
                       and type(c.get("mmr_claim")) in (int, float)]
            if not ratings or max(ratings) < 2000:
                raise ValueError(f"qualified player has no exact MMR claim: {item['game_id']}")
            mmr = max(ratings)
            if item["game_id"] in games:
                raise ValueError(f"duplicate perspective key: {item['game_id']}")
            games[item["game_id"]] = dict(split=item["split"], matchup=item["matchup"],
                                          replay_sha256=item["replay_sha256"],
                                          perspective=perspective, mmr_claim=mmr,
                                          actor_group=actor_group(players[perspective]))
            bucket = counts[item["split"]][item["matchup"]]
            bucket["total"] += 1
            for threshold in (2300, 2500, 2700):
                bucket[f"mmr_ge_{threshold}"] += mmr >= threshold
    return dict(schema=SCHEMA, identity_sha256=sha256(identity_path),
                audit_path=str(audit_path.resolve()),
                quality="exact-player catalog MMR claim; professional identity unverified",
                counts={s: {m: dict(c) for m, c in matchups.items()}
                        for s, matchups in counts.items()}, games=games)


def load_index(path, identity_path):
    index = json.loads(Path(path).read_text(encoding="utf8"))
    if index.get("schema") != SCHEMA or index.get("identity_sha256") != sha256(identity_path):
        raise ValueError("quality index does not match frozen release identity")
    selected = json.loads(Path(identity_path).read_text(encoding="utf8"))["selected"]
    if set(index["games"]) != {row["game_id"] for row in selected}:
        raise ValueError("quality index game set differs from release")
    for row in selected:
        quality = index["games"][row["game_id"]]
        if any(quality[field] != row[field] for field in
               ("split", "matchup", "replay_sha256", "perspective")):
            raise ValueError(f"quality index perspective differs: {row['game_id']}")
        if (type(quality.get("actor_group")) is not str or len(quality["actor_group"]) != 64
                or type(quality.get("mmr_claim")) not in (int, float)
                or quality["mmr_claim"] < 2000):
            raise ValueError(f"invalid quality data: {row['game_id']}")
    return index


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--identity", type=Path, required=True)
    parser.add_argument("--audit", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    index = build_index(args.identity, args.audit)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(index, indent=2) + "\n", encoding="utf8")
    print(json.dumps(dict(output=str(args.output.resolve()), counts=index["counts"]), indent=2))


if __name__ == "__main__":
    main()
