"""Extract training-only whole-game observation pilots; never launches model fitting."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import subprocess
import shutil
import time

from .replay_assets import verify
from .whole_game_actor_dedupe import canonicalize_actor_selection
from .whole_game_labels import COMMAND_SCHEMA, label_command, materialize, observation_reference

SCHEMA = "protodd-whole-game-pilot-v3.2"


def sha256(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def select_games(manifest, per_matchup=1):
    if type(per_matchup) is not int or per_matchup < 1:
        raise ValueError("positive per-matchup count required")
    selected, counts = [], Counter()
    for game in manifest["games"]:
        # Inspect metadata only. No held-out replay or tensor is opened.
        if game["split"] != "train":
            continue
        for perspective, (race, quality) in enumerate(zip(game["races"], game["player_quality"])):
            matchup = "Pv" + game["races"][1 - perspective]
            if race == "P" and quality == "qualified_ladder" and counts[matchup] < per_matchup:
                selected.append(dict(game=game, perspective=perspective, matchup=matchup))
                counts[matchup] += 1
                break  # one qualified perspective per game for this bounded pilot
        if all(counts[m] == per_matchup for m in ("PvT", "PvZ", "PvP")):
            return selected
    raise ValueError("insufficient qualified training games for all three matchups")


def analyse(directory):
    directory = Path(directory)
    summary = json.loads((directory / "summary.json").read_text())
    if (summary.get("schema") != SCHEMA or summary.get("complete") is not True
            or summary.get("command_schema") != COMMAND_SCHEMA):
        raise ValueError("incomplete pilot")
    terrain = json.loads((directory / "terrain.json").read_text())
    walktiles = summary["width_tiles"] * summary["height_tiles"] * 16
    if (terrain.get("schema") != "protodd-terrain-v2" or terrain["width_walktiles"] != summary["width_tiles"] * 4
            or terrain["height_walktiles"] != summary["height_tiles"] * 4
            or len(terrain["walkability"]) != walktiles or len(terrain["height"]) != walktiles
            or set(terrain["walkability"]) - set("01") or set(terrain["height"]) - set("012")):
        raise ValueError("invalid static terrain")
    references, last_frame, next_sequence, counts = {}, {}, {}, Counter()
    seen_ids = {}
    with (directory / "observations.jsonl").open() as stream:
        for line in stream:
            row = json.loads(line)
            p, frame, sequence = row["perspective"], row["frame"], row["sequence"]
            if (row["schema"] != SCHEMA or sequence != next_sequence.get(p, 0)
                    or frame < last_frame.get(p, 0) or frame > summary["valid_through_frame"]):
                raise ValueError("noncausal observation sequence")
            next_sequence[p] = sequence + 1
            last_frame[p] = frame
            if (len(row["vision"]) != summary["width_tiles"] * summary["height_tiles"]
                    or set(row["vision"]) - set("012")):
                raise ValueError("invalid vision grid")
            entities = row["entities"]
            if len({e["id"] for e in entities}) != len(entities):
                raise ValueError("duplicate entity IDs")
            known = seen_ids.setdefault(p, set())
            known.update(e["id"] for e in entities)
            own = set()
            for entity in entities:
                if (entity["id"] < 0 or entity["relation"] not in (0, 1, 2)
                        or not 0 <= entity["first_seen"] <= entity["last_seen"] <= frame
                        or (entity["visible"] and entity["last_seen"] != frame)):
                    raise ValueError("invalid entity history")
                private = entity["own_state"]
                if entity["relation"] != 0:
                    if private is not None:
                        raise ValueError("enemy/neutral private state leaked")
                    counts["visible_enemy_rows" if entity["visible"] else "remembered_enemy_rows"] += entity["relation"] == 1
                else:
                    if not isinstance(private, dict):
                        raise ValueError("own state missing")
                    own.add(entity["id"])
                    if private["order_target"] != -1 and private["order_target"] not in known:
                        raise ValueError("order target was never observed")
                    if not set(private["cargo"]).issubset(known):
                        raise ValueError("cargo was never observed")
            if row["reason"] == "before_command":
                references[p, sequence] = observation_reference(row)
            elif row["reason"] != "cadence":
                raise ValueError("unknown observation reason")
            counts["observations"] += 1
            for name, size in (("technology_completed", summary["technology_count"]),
                               ("technology_in_progress", summary["technology_count"]),
                               ("upgrade_levels", summary["upgrade_count"]),
                               ("upgrade_in_progress", summary["upgrade_count"])):
                if len(row[name]) != size or any(type(v) is not int or v < 0 or (name != "upgrade_levels" and v > 1) for v in row[name]):
                    raise ValueError("invalid own technology/upgrade state")
    codes, evidence, candidates = Counter(), Counter(), Counter()
    with (directory / "commands.jsonl").open() as stream:
        for line in stream:
            row = json.loads(line)
            row, _ = canonicalize_actor_selection(row)
            reference = references.pop((row["perspective"], row["observation_sequence"]), None)
            if reference is None:
                raise ValueError("command lacks causal own-actor observation")
            candidate = label_command(row, reference, summary["width_tiles"] * 32, summary["height_tiles"] * 32)
            evidence[row["acceptance"]] += 1
            if candidate:
                candidates[candidate["actions"]["kind"]] += 1
            bytes.fromhex(row["payload_hex"])
            codes[str(row["code"])] += 1
            counts["commands"] += 1
    if references or any(counts[k] != summary[k] for k in ("observations", "commands")):
        raise ValueError("missing/truncated observation-command pairs")
    return dict(counts=counts, command_codes=codes, command_evidence=evidence, candidate_kinds=candidates,
                training_ready=False, acceptance="immediate own command-state transitions; completion remains unverified",
                remaining_gates=["live BWAPI parity", "remaining observation/ability coverage",
                                 "command completion feedback", "shared runtime encoder", "multi-head model and executor"])


def run(args):
    manifest_path = args.release / "manifest.json"
    release = json.loads((args.release / "release.json").read_text())
    if not release.get("complete") or sha256(manifest_path) != release["manifest_sha256"]:
        raise ValueError("data release is incomplete or changed")
    selected = select_games(json.loads(manifest_path.read_text()), args.per_matchup)
    verify(args.assets)
    args.output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[1]
    source_files = [Path(__file__), root / "training/whole_game_labels.py", *sorted((root / "tools/replay_native").glob("*.hpp")),
                    *sorted((root / "tools/replay_native").glob("*.cpp")),
                    root / "tools/replay_native/CMakeLists.txt", root / "include/protodd/WholeGameObservation.hpp"]
    paths = [*source_files, args.extractor, args.selftest, args.decoder, args.reference,
             *(args.mpq / name for name in ("StarDat.mpq", "BrooDat.mpq", "Patch_rt.mpq"))]
    identity = dict(schema=SCHEMA, manifest_sha256=sha256(manifest_path), selected=selected,
                    hashes={str(p.resolve()): sha256(p) for p in paths}, training_ready=False)
    (args.output / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
    for source in source_files:
        target = args.output / "source" / source.resolve().relative_to(root)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    reports = []
    for index, record in enumerate(selected):
        start = time.monotonic()
        game, perspective = record["game"], record["perspective"]
        replay = (args.root / game["path"]).resolve()
        if not replay.is_relative_to(args.root.resolve()) or sha256(replay) != game["replay_sha256"]:
            raise ValueError("replay identity/path mismatch")
        work = args.output / f"{index:02d}-{record['matchup']}"
        work.mkdir()
        raw = work / "replay.raw"
        prefix = min(game["valid_through_frame"], args.max_frames)
        for name, command in [
            ("decode", [args.decoder, replay, raw]),
            ("selftest", [args.selftest, args.mpq, args.assets, raw, perspective]),
            ("extract", [args.extractor, args.mpq, args.assets, raw, work / "extracted", prefix, perspective]),
            ("reference", [args.reference, args.mpq, args.assets, raw, 240]),
        ]:
            with (work / f"{name}.stdout.log").open("wb") as stdout, (work / f"{name}.stderr.log").open("wb") as stderr:
                subprocess.run([str(v) for v in command], stdout=stdout, stderr=stderr,
                               timeout=args.timeout, check=True)
        report = analyse(work / "extracted")
        report["labels"] = materialize(work / "extracted", work / "imitation-labels.jsonl")
        checkpoints = {r["frame"]: r for r in map(json.loads, (work / "extracted/checkpoints.jsonl").read_text().splitlines())}
        compared = set()
        with (work / "reference.stdout.log").open() as stream:
            for line in stream:
                row = json.loads(line)
                frame = row["frame"]
                if frame in checkpoints:
                    if checkpoints[frame] != row:
                        raise ValueError(f"instrumentation changed playback at frame {frame}")
                    compared.add(frame)
        expected = {f for f in checkpoints if f % 240 == 0}
        if not expected or not expected.issubset(compared):
            raise ValueError("missing reference checkpoints")
        report.update(game_id=game["game_id"], matchup=record["matchup"], perspective=perspective,
                      split="train", valid_through_frame=prefix, checkpoints_matched=len(compared),
                      original_game_parity=False, elapsed_seconds=round(time.monotonic() - start, 2))
        report["artifact_sha256"] = {str(p.relative_to(work)): sha256(p) for p in (work / "extracted").iterdir() if p.is_file()}
        report["artifact_sha256"]["imitation-labels.jsonl"] = sha256(work / "imitation-labels.jsonl")
        (work / "report.json").write_text(json.dumps(report, indent=2) + "\n")
        reports.append(report)
        print(json.dumps(report), flush=True)
    # Fail if the source/binary changed while the pilot ran.
    if any(sha256(Path(p)) != digest for p, digest in identity["hashes"].items()):
        raise ValueError("pilot input changed during execution")
    result = dict(complete=True, schema=SCHEMA, training_ready=False, reports=reports)
    (args.output / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("release", "root", "extractor", "selftest", "decoder", "reference", "mpq", "assets", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--per-matchup", type=int, default=1)
    parser.add_argument("--max-frames", type=int, default=24000)
    parser.add_argument("--timeout", type=int, default=600)
    args = parser.parse_args()
    if args.max_frames < 1 or args.timeout < 1:
        parser.error("positive frame cap and timeout required")
    run(args)


if __name__ == "__main__":
    main()
