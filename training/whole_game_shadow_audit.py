"""Audit an isolated embedded-model shadow campaign without claiming strength."""
from __future__ import annotations

import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import statistics


SCHEMA = "protodd-whole-game-shadow-audit-v1"

POSITION_KINDS = {"move", "attack_move", "patrol", "unload_position", "build"}
ENTITY_KINDS = {"follow", "repair", "gather", "load", "unload_unit",
                "merge_archon", "merge_dark_archon"}
NONE_KINDS = {"train", "morph", "research", "upgrade", "cancel_queue",
              "cancel_build", "cancel_morph", "cancel_research", "cancel_upgrade",
              "cancel_addon", "stop", "return_cargo", "cloak", "decloak",
              "siege", "unsiege", "train_fighter", "hold", "burrow", "unburrow",
              "cancel_nuke", "liftoff", "stim"}


def intent_shape_compatible(sample):
    if len(sample) not in (15, 16):
        return False
    kind, mode = sample[2], sample[5]
    if kind in POSITION_KINDS:
        return mode == "position" and int(sample[9]) >= 0 and int(sample[10]) >= 0
    if kind in ENTITY_KINDS:
        return mode == "entity" and int(sample[8]) >= 0
    if kind in NONE_KINDS:
        return mode == "none"
    if kind in {"right_click", "attack", "rally"}:
        return mode in {"entity", "position"}
    return True  # Spells and raw orders need a later BWAPI legality check.


def intent_breakdown(samples):
    """Expose dominant failure families before promoting a model to control."""
    if samples is None:
        return None
    kinds = Counter(sample[2] for sample in samples)
    modes = Counter(sample[5] for sample in samples)
    shape_by_kind = Counter(sample[2] for sample in samples if intent_shape_compatible(sample))
    legal_by_kind = Counter(sample[2] for sample in samples
                            if len(sample) == 16 and int(sample[15]) > 0)
    return dict(kinds=dict(sorted(kinds.items())),
                target_modes=dict(sorted(modes.items())),
                shape_compatible_by_kind=dict(sorted(shape_by_kind.items())),
                legal_by_kind=dict(sorted(legal_by_kind.items())))


def valid_intent_samples(samples):
    if not samples:
        return False
    try:
        for sample in samples:
            if len(sample) not in (15, 16) or not 0 <= float(sample[1]) <= 1 or \
                    not 0 <= float(sample[3]) <= 1 or int(sample[6]) < 1 or \
                    int(sample[7]) < 0:
                return False
            for index in (0, 8, 9, 10, 11, 12, 13, 14):
                int(sample[index])
            if len(sample) == 16 and int(sample[15]) < 0:
                return False
    except (ValueError, IndexError):
        return False
    return True


def summarize(campaign: Path, *, require_intents=False) -> dict:
    campaign = Path(campaign)
    manifest = json.loads((campaign / "manifest.json").read_text(encoding="utf8"))
    games = [json.loads(line) for line in (campaign / "server/games.jsonl").read_text().splitlines()]
    reports = [json.loads(line) for line in (campaign / "server/results.jsonl").read_text().splitlines()]
    by_game = {}
    for report in reports:
        by_game.setdefault(report["gameID"], []).append(report)
    rows = []
    for game in games:
        game_id = game["gameID"]
        pair = by_game.get(game_id, [])
        own = [entry for entry in pair if entry["reportingBot"] == manifest["bot"]]
        other = [entry for entry in pair if entry["reportingBot"] != manifest["bot"]]
        if len(own) != 1 or len(other) != 1:
            rows.append(dict(game_id=game_id, complete=False, reason="unpaired tournament report"))
            continue
        received = (campaign / "server/replays/bot-write" / f"game-{game_id}" /
                    manifest["bot"] / "received")
        if (received / "WholeGame-controller.txt").exists():
            rows.append(dict(game_id=game_id, complete=False,
                             reason="learned control was enabled; use controller evaluation"))
            continue
        shadow = received / "WholeGame-shadow.csv"
        log = received / "Protodd.log"
        if not shadow.is_file() or not log.is_file():
            rows.append(dict(game_id=game_id, complete=False, reason="missing bot trace"))
            continue
        with shadow.open(newline="", encoding="utf8") as stream:
            samples = list(csv.reader(stream))
        if not samples or any(len(sample) != 6 for sample in samples):
            rows.append(dict(game_id=game_id, complete=False, reason="missing or malformed model samples"))
            continue
        frames = [int(sample[0]) for sample in samples]
        times = sorted(float(sample[3]) for sample in samples)
        performance = None
        errors = []
        with log.open(encoding="utf8", errors="replace") as stream:
            for line in stream:
                if line.startswith("PERF_SUMMARY,"):
                    performance = line.strip().split(",")
                elif line.startswith("ERROR,"):
                    errors.append(line.strip()[:240])
        model_error = received / "WholeGame-model-error.txt"
        if model_error.exists():
            errors.append(model_error.read_text(encoding="utf8", errors="replace")[:240])
        intent_path = received / "WholeGame-intents.csv"
        intent_samples = None
        if intent_path.is_file():
            with intent_path.open(newline="", encoding="utf8") as stream:
                intent_samples = list(csv.reader(stream))
            if not valid_intent_samples(intent_samples):
                errors.append("missing or malformed decoded intent samples")
                intent_samples = None
        elif require_intents:
            errors.append("missing decoded intent trace")
        timers = own[0].get("timers", [])
        timeout_frames = sum(int(item.get("frameCount", 0)) for item in timers)
        max_callback_ms = float(performance[3]) if performance and len(performance) >= 4 else None
        healthy = (not errors and all(entry.get("gameEndType") == "NORMAL" and
                    not entry.get("crash") and not entry.get("gameTimeout") for entry in pair)
                   and timeout_frames == 0 and max_callback_ms is not None and
                   max_callback_ms < 42 and frames == sorted(set(frames)))
        rows.append(dict(game_id=game_id, complete=True, healthy=healthy,
                         won=own[0].get("won"), model_ticks=len(samples),
                         first_frame=frames[0], last_frame=frames[-1],
                         model_inference_ms=dict(median=statistics.median(times),
                                                 p95=times[min(len(times) - 1, int(.95 * len(times)))],
                                                 maximum=times[-1]),
                         max_callback_ms=max_callback_ms,
                         max_entities=max(int(sample[1]) for sample in samples),
                         max_overflow=max(int(sample[2]) for sample in samples),
                         intent_ticks=len(intent_samples) if intent_samples is not None else None,
                         intent_shape_compatible=sum(intent_shape_compatible(sample)
                             for sample in intent_samples) if intent_samples is not None else None,
                         intents_with_legal_commands=sum(len(sample) == 16 and int(sample[15]) > 0
                             for sample in intent_samples) if intent_samples is not None else None,
                         intent_breakdown=intent_breakdown(intent_samples),
                         timeout_frames=timeout_frames, errors=errors))
    return dict(schema=SCHEMA, campaign=str(campaign.resolve()),
                scheduled=len(games), audited=len(rows),
                healthy_shadow=all(row.get("healthy") for row in rows),
                controller="existing bot; model inference only", games=rows)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("campaign", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--require-intents", action="store_true")
    args = parser.parse_args()
    result = summarize(args.campaign, require_intents=args.require_intents)
    encoded = json.dumps(result, indent=2) + "\n"
    if args.output:
        args.output.write_text(encoded, encoding="utf8")
    print(encoded, end="")


if __name__ == "__main__":
    main()
