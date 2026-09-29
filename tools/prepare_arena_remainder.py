"""Prepare an isolated continuation for wholly unreported arena game IDs.

Finished games and any failed game's partial files stay in the original run.
The continuation retains the original game ID so the local seed override and
schedule identity remain fixed.
"""

from __future__ import annotations

import argparse
from collections import Counter
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.arena import inspect, prepare, verify
from training.schema import sha256


def prepare_remainder(original: Path, output: Path) -> dict:
    original = original.resolve()
    verify(original)
    manifest = json.loads((original / "manifest.json").read_text())
    settings = json.loads((original / "server/server_settings.json").read_text())
    schedule = [json.loads(row) for row in
                (original / "server/games.jsonl").read_text().splitlines() if row]
    result_path = original / "server/results.jsonl"
    results = [json.loads(row) for row in
               (result_path.read_text().splitlines() if result_path.is_file() else [])
               if row]
    counts = Counter(row["gameID"] for row in results)
    if any(count not in (0, 2) for count in (counts[game["gameID"]] for game in schedule)):
        raise ValueError("partial report pair needs manual recovery")
    reviewed = inspect(original)
    valid_ids = {row["game_id"] for row in reviewed["structurally_valid"]}
    if any(counts[game["gameID"]] == 2 and game["gameID"] not in valid_ids
           for game in schedule):
        raise ValueError("completed report pair failed structural review")
    missing = [game for game in schedule if counts[game["gameID"]] == 0]
    if not missing:
        raise ValueError("all scheduled games already reported")
    opponents = [bot["BotName"] for bot in settings["bots"]
                 if bot["BotName"] != manifest["bot"]]
    maps = settings["maps"]
    if len(schedule) % (len(opponents) * len(maps)):
        raise ValueError("schedule does not match whole paired rounds")
    rounds = len(schedule) // (len(opponents) * len(maps))
    prepared = prepare(Path(manifest["template"]), output,
                       original / "server/bots" / manifest["bot"] / "AI" /
                       f"{manifest['bot']}.dll", opponents, maps,
                       purpose=manifest["purpose"], race=manifest["race"],
                       rounds=rounds, port=settings["serverPort"],
                       server_jar=original / "server/server.jar",
                       client_bundle=original / "client1",
                       policy_mode=(original / "server/bots" / manifest["bot"] /
                                    "read/Policy-mode.txt").read_text().strip())
    regenerated = [json.loads(row) for row in
                   (output / "server/games.jsonl").read_text().splitlines() if row]
    if regenerated != schedule:
        raise ValueError("regenerated schedule differs from original")
    changed = {name for name in manifest["components"].keys() | prepared["components"].keys()
               if manifest["components"].get(name) != prepared["components"].get(name)}
    if changed:
        raise ValueError(f"nonmatching frozen inputs: {sorted(changed)}")
    schedule_path = output / "server/games.jsonl"
    schedule_path.write_text("".join(json.dumps(game) + "\n" for game in missing))
    prepared["components"]["server/games.jsonl"] = sha256(schedule_path)
    prepared["games"] = len(missing)
    prepared["continuation_of"] = str(original)
    prepared["continuation_manifest_sha256"] = sha256(original / "manifest.json")
    prepared["continued_game_ids"] = [game["gameID"] for game in missing]
    (output / "manifest.json").write_text(json.dumps(prepared, indent=2) + "\n")
    verify(output)
    return {"output": str(output.resolve()),
            "continued_game_ids": prepared["continued_game_ids"]}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("original", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(prepare_remainder(args.original, args.output), indent=2))
