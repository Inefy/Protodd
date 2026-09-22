"""Describe a legacy replay run and optionally audit training label coverage.

This is read-only with respect to the source run. Final-test result/target files
are never opened. Metadata mode reads cohort/run metadata and selected result
summaries; labels mode additionally streams the selected gzip files. Validation
requires --include-validation. No mode permits final-test label analysis.
"""
import argparse
from collections import Counter, defaultdict
from datetime import datetime, timezone
import gzip
import hashlib
import io
from itertools import groupby
import json
from pathlib import Path
import re
import time
import zlib

from .prepare import QUALITIES, validate_sample
from .schema import load_schema

MAX_JSON_BYTES = 64 * 1024 * 1024
MAX_LINE_CHARS = 1024 * 1024
WINDOW = 24
RECORDED_FIELDS = ("samples", "positive_samples", "masked_windows",
                   "accepted_commands", "rejected_commands", "repeated_build_commands")


def utc_now():
    return datetime.now(timezone.utc).isoformat()


def read_json(path, limit=MAX_JSON_BYTES, with_digest=False):
    path = Path(path)
    if path.resolve() != path.absolute():
        raise ValueError(f"Artifact path redirects outside its recorded identity: {path.name}")
    with path.open("rb") as stream:
        data = stream.read(limit + 1)
    if len(data) > limit:
        raise ValueError(f"Metadata exceeds the bounded {limit}-byte limit: {path.name}")
    value = json.loads(data)
    if not isinstance(value, dict):
        raise ValueError(f"Metadata root must be an object: {path.name}")
    return (value, hashlib.sha256(data).hexdigest()) if with_digest else value


def write_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    temporary.replace(path)


def nonnegative(value, name):
    if type(value) is not int or value < 0:
        raise ValueError(f"Invalid nonnegative integer: {name}")
    return value


def digest_name(value, name):
    if not isinstance(value, str) or not re.fullmatch("[0-9a-f]{64}", value):
        raise ValueError(f"Invalid {name}")
    return value


def qualified_perspectives(game):
    return {i: game["slots"][i] for i in range(2)
            if game["races"][i] == "P" and game["player_quality"][i] in QUALITIES}


def matchup(game):
    return "Pv" + next((r for r in game["races"] if r != "P"), "P")


def validate_cohort(cohort):
    games = cohort.get("games")
    if not isinstance(games, list) or not games:
        raise ValueError("Cohort must contain games")
    ids, hashes, groups = set(), set(), {}
    for game in games:
        gid = game.get("game_id")
        if not isinstance(gid, str) or not re.fullmatch(r"[A-Za-z0-9_.:-]{1,160}", gid):
            raise ValueError("Invalid game identity")
        digest_name(game.get("replay_sha256"), "replay_sha256")
        group = game.get("duplicate_group")
        if not isinstance(group, str) or not group:
            raise ValueError("Missing duplicate group")
        split = game.get("split")
        if split not in ("train", "validation", "test"):
            raise ValueError("Invalid split")
        if gid in ids or game["replay_sha256"] in hashes:
            raise ValueError("Duplicate game or replay identity")
        if group in groups and groups[group] != split:
            raise ValueError("Duplicate group crosses splits")
        ids.add(gid)
        hashes.add(game["replay_sha256"])
        groups[group] = split
        if (len(game.get("races", [])) != 2 or "P" not in game["races"] or
                any(r not in ("P", "T", "Z") for r in game["races"]) or
                len(game.get("slots", [])) != 2 or len(set(game["slots"])) != 2 or
                any(type(s) is not int or not 0 <= s < 12 for s in game["slots"]) or
                len(game.get("player_quality", [])) != 2):
            raise ValueError("Invalid race, slot or perspective identity")
        nonnegative(game.get("valid_through_frame"), "valid_through_frame")
        if split == "train" and (game.get("map_holdout") or game.get("player_holdout")):
            raise ValueError("A declared held-out game entered training")
        if not qualified_perspectives(game):
            raise ValueError("No qualified Protoss perspective")
    return games


class Progress:
    def __init__(self, output):
        self.output = output
        self.state = {}
        self.last_write = 0.0

    def update(self, force=False, **fields):
        self.state.update(fields)
        if force or time.monotonic() - self.last_write >= 2:
            write_json(self.output / "progress.json", {**self.state, "updated_at": utc_now()})
            self.last_write = time.monotonic()


class HashingReader:
    """Hash the compressed bytes during the same forward-only gzip read."""
    def __init__(self, stream):
        self.stream = stream
        self.digest = hashlib.sha256()

    def read(self, count=-1):
        data = self.stream.read(count)
        self.digest.update(data)
        return data


def stream_jsonl(path, expected_hash, integrity):
    if path.resolve() != path.absolute():
        raise ValueError(f"Artifact path redirects outside its recorded identity: {path.name}")
    if expected_hash is not None:
        digest_name(expected_hash, path.name + " sha256")
    with path.open("rb") as raw:
        hashed = HashingReader(raw)
        with gzip.GzipFile(fileobj=hashed, mode="rb") as compressed:
            with io.TextIOWrapper(compressed, encoding="utf-8") as stream:
                number = 0
                while True:
                    line = stream.readline(MAX_LINE_CHARS + 1)
                    if not line:
                        break
                    number += 1
                    if len(line) > MAX_LINE_CHARS:
                        raise ValueError(f"Oversized JSONL row: {path.name}:{number}")
                    if not line.strip():
                        raise ValueError(f"Empty JSONL row: {path.name}:{number}")
                    row = json.loads(line)
                    if not isinstance(row, dict):
                        raise ValueError(f"Non-object JSONL row: {path.name}:{number}")
                    yield row
        actual_hash = hashed.digest.hexdigest()
        if expected_hash is not None and actual_hash != expected_hash:
            raise ValueError(f"Compressed artifact hash mismatch: {path.name}")
        integrity[path.name] = "verified" if expected_hash else "unknown_missing_hash"


def recorded_metrics(game, result, schema):
    if result.get("game_id") != game["game_id"]:
        raise ValueError("Result belongs to another game")
    counts = result.get("counts")
    if counts is not None:
        if not isinstance(counts, dict) or any(a not in schema["actions"] for a in counts):
            raise ValueError("Invalid recorded action names")
        for action, value in counts.items():
            nonnegative(value, action)
        if result.get("samples") != sum(counts.values()):
            raise ValueError("Recorded action counts do not sum to recorded samples")
    stats = result.get("stats", {})
    if not isinstance(stats, dict) or not isinstance(stats.get("perspectives", []), list):
        raise ValueError("Invalid extractor statistics object")
    for field, expected in (("schema", schema["version"]), ("fingerprint", schema["fingerprint"]),
                            ("end_frame", game["valid_through_frame"])):
        if field in stats and stats[field] != expected:
            raise ValueError("Result schema or replay duration differs from cohort")
    perspectives = {}
    for p in stats.get("perspectives", []):
        index = p.get("perspective")
        if (type(index) is not int or index not in (0, 1) or index in perspectives or
                p.get("slot") != game["slots"][index] or game["races"][index] != "P"):
            raise ValueError("Result perspective/slot identity differs from cohort")
        perspectives[index] = p
    eligible = qualified_perspectives(game)
    metrics = {}
    for name in RECORDED_FIELDS:
        values = [perspectives.get(i, {}).get(name) for i in eligible]
        metrics[name] = (sum(nonnegative(v, name) for v in values)
                         if all(v is not None for v in values) else None)
    if metrics["samples"] is not None and result.get("samples") != metrics["samples"]:
        raise ValueError("Qualified perspective sample counts differ from result")
    if counts is not None and metrics["positive_samples"] is not None:
        if sum(counts.values()) - counts.get("wait", 0) != metrics["positive_samples"]:
            raise ValueError("Qualified positive label counts differ from result")
    return {"metrics": metrics, "action_counts": counts,
            "schema_recorded": all(k in stats for k in ("schema", "fingerprint", "end_frame"))}


def audit_labels(game, directory, result, recorded, schema, progress):
    """Merge two ordered streams using only the current 24-frame window."""
    if game["split"] == "test":
        raise ValueError("Final-test labels are forbidden")
    eligible = qualified_perspectives(game)
    owners = set(eligible.values())
    observed, labels, delays = Counter(), Counter(), Counter()
    phases = defaultdict(Counter)
    integrity = {}
    last_samples = {}

    def sample_rows():
        previous_frame = -1
        for row in stream_jsonl(directory / "samples.jsonl.gz", result.get("samples_sha256"), integrity):
            validate_sample(row, {game["game_id"]: game}, schema)
            perspective, frame = row["perspective"], row["frame"]
            if (perspective not in eligible or frame % WINDOW or frame < previous_frame or
                    frame <= last_samples.get(perspective, -1) or
                    not frame <= row["action_frame"] < frame + WINDOW or
                    frame + WINDOW > game["valid_through_frame"]):
                raise ValueError("Noncausal, unordered, duplicate or incomplete sample window")
            last_samples[perspective] = frame
            previous_frame = frame
            observed["samples"] += 1
            labels[row["action"]] += 1
            phases["before_5m" if frame < 7200 else "5m_to_12m" if frame < 17280 else "after_12m"][row["action"]] += 1
            if row["action"] != "wait":
                observed["positive_samples"] += 1
                delays[str(row["action_frame"] - frame)] += 1
            if observed["samples"] % 4096 == 0:
                progress.update(current_game_samples=observed["samples"], current_game=game["game_id"])
            yield row

    def action_rows():
        previous_frame = -1
        rows_read = 0
        for row in stream_jsonl(directory / "actions.jsonl.gz", result.get("actions_sha256"), integrity):
            frame = nonnegative(row.get("frame"), "action frame")
            if (frame < previous_frame or frame > game["valid_through_frame"] or
                    row.get("action") not in schema["actions"][1:] or
                    type(row.get("owner")) is not int or not 0 <= row["owner"] < 12 or
                    type(row.get("accepted")) is not bool or type(row.get("repeated")) is not bool or
                    type(row.get("producer")) is not int or
                    (row["accepted"] and (row["repeated"] or row["producer"] < 0))):
                raise ValueError("Invalid accepted-command trace or ordering")
            previous_frame = frame
            rows_read += 1
            if rows_read % 4096 == 0:
                progress.update(current_game_actions=rows_read, current_game=game["game_id"])
            if row["owner"] not in owners:
                observed["ignored_other_perspective_commands"] += 1
                continue
            observed["accepted_commands" if row["accepted"] else "repeated_build_commands"
                     if row["repeated"] else "rejected_commands"] += 1
            yield row

    def sample_windows():
        for frame, rows in groupby(sample_rows(), lambda r: r["frame"]):
            yield frame, {eligible[r["perspective"]]: r for r in rows}

    def action_windows():
        for frame, rows in groupby(action_rows(), lambda r: r["frame"] // WINDOW * WINDOW):
            summary = {}
            for row in rows:
                if row["accepted"]:
                    item = summary.setdefault(row["owner"], {"count": 0, "first": row})
                    item["count"] += 1
            yield frame, summary

    sample_iterator, action_iterator = iter(sample_windows()), iter(action_windows())
    sample, action = next(sample_iterator, None), next(action_iterator, None)
    while sample is not None or action is not None:
        frame = min(item[0] for item in (sample, action) if item is not None)
        sample_window = sample[1] if sample is not None and sample[0] == frame else {}
        action_window = action[1] if action is not None and action[0] == frame else {}
        for owner in sample_window.keys() | action_window.keys():
            row, commands = sample_window.get(owner), action_window.get(owner)
            count = commands["count"] if commands else 0
            if row is not None:
                expected_action = commands["first"]["action"] if commands else "wait"
                expected_frame = commands["first"]["frame"] if commands else frame
                if row["action"] != expected_action or row["action_frame"] != expected_frame:
                    raise ValueError("Label does not match first accepted mapped command")
                if count:
                    observed["accepted_commands_in_retained_windows"] += count
                    observed["additional_accepted_commands_in_retained_windows"] += count - 1
                    observed["retained_windows_with_multiple_accepted_commands"] += count > 1
            elif count:
                observed["accepted_commands_without_retained_window"] += count
                if frame + WINDOW > game["valid_through_frame"]:
                    observed["accepted_commands_in_incomplete_terminal_window"] += count
        if sample is not None and sample[0] == frame:
            sample = next(sample_iterator, None)
        if action is not None and action[0] == frame:
            action = next(action_iterator, None)
    if observed["samples"] == 0:
        raise ValueError("Extracted game contains no usable samples")
    if recorded["action_counts"] is not None and labels != Counter(recorded["action_counts"]):
        raise ValueError("Streamed label counts differ from recorded counts")
    for field in RECORDED_FIELDS:
        if field != "masked_windows" and recorded["metrics"][field] is not None:
            if observed[field] != recorded["metrics"][field]:
                raise ValueError(f"Streamed {field} differs from recorded qualified-perspective total")
    observed["unretained_accepted_commands"] = observed["accepted_commands"] - observed["positive_samples"]
    return {"metrics": dict(observed), "action_counts": dict(labels), "action_delay_frames": dict(delays),
            "phase_action_counts": dict(phases), "integrity": integrity,
            "hashes_verified": all(v == "verified" for v in integrity.values()) and len(integrity) == 2}


def analyze(input_run, output, mode="metadata", include_validation=False, max_games=None):
    source, output = Path(input_run).resolve(), Path(output).resolve()
    if mode not in ("metadata", "labels") or (max_games is not None and (type(max_games) is not int or max_games < 1)):
        raise ValueError("Invalid mode or max_games")
    if output == source or source in output.parents:
        raise ValueError("Analysis output must be outside the immutable source run")
    output.mkdir(parents=True, exist_ok=False)
    progress = Progress(output)
    progress.update(force=True, stage="cohort_inventory", status="running")
    try:
        cohort, cohort_digest = read_json(source / "cohort.json", with_digest=True)
        games = validate_cohort(cohort)
        schema = load_schema()
        splits = {"train", "validation"} if include_validation else {"train"}
        inventory = {name: Counter() for name in ("splits", "matchups", "split_matchups", "player_qualities",
                                                  "qualified_perspectives", "map_names", "duration_frames")}
        for game in games:
            inventory["splits"][game["split"]] += 1
            inventory["matchups"][matchup(game)] += 1
            inventory["split_matchups"][game["split"] + "/" + matchup(game)] += 1
            inventory["player_qualities"].update(q for r, q in zip(game["races"], game["player_quality"]) if r == "P")
            inventory["qualified_perspectives"][game["split"]] += len(qualified_perspectives(game))
            inventory["map_names"][str(game.get("map_name", "unknown"))] += 1
            end = game["valid_through_frame"]
            inventory["duration_frames"]["under_4320" if end < 4320 else "4320_to_7199" if end < 7200
                                          else "7200_to_17279" if end < 17280 else "17280_to_43199"
                                          if end < 43200 else "43200_and_over"] += 1
        eligible = [g for g in games if g["split"] in splits]
        selected = eligible if max_games is None else eligible[:max_games]
        source_status = read_json(source / "status.json", 1024 * 1024) if (source / "status.json").exists() else {}
        source_counts = {k: source_status.get(k) for k in ("cohort_games", "extracted_games", "quarantined_games")}
        known_source_counts = all(type(v) is int and v >= 0 for v in source_counts.values())
        source_complete = (source_counts["cohort_games"] == len(games) and
                           source_counts["extracted_games"] + source_counts["quarantined_games"] == len(games)) if known_source_counts else None
        statuses, exclusions, actions, observed, observed_actions = Counter(), Counter(), Counter(), Counter(), Counter()
        by_split, by_matchup = defaultdict(Counter), defaultdict(Counter)
        status_by_split, status_by_matchup = defaultdict(Counter), defaultdict(Counter)
        phase_counts, delays = defaultdict(Counter), Counter()
        recorded_totals = {k: {"total_observed": 0, "games_with_evidence": 0, "games_without_evidence": 0} for k in RECORDED_FIELDS}
        errors, label_games, unverified_games, action_metadata_games = [], 0, 0, 0
        with (output / "games.jsonl").open("w", encoding="utf-8") as details:
            for index, game in enumerate(selected, 1):
                progress.update(force=index == 1, stage="label_coverage" if mode == "labels" else "result_inventory",
                                games_processed=index - 1, games_selected=len(selected), current_game=game["game_id"],
                                current_game_samples=0, current_game_actions=0)
                item = {"game_id": game["game_id"], "split": game["split"], "matchup": matchup(game)}
                directory = source / "games" / game["replay_sha256"]
                # A source junction must not redirect these reads into another run.
                if source not in directory.resolve().parents:
                    raise ValueError("Game artifact directory escapes the source run")
                try:
                    result = read_json(directory / "result.json", 1024 * 1024)
                    status = result.get("status")
                    if status == "quarantined":
                        if result.get("game_id", game["game_id"]) != game["game_id"]:
                            raise ValueError("Quarantine result belongs to another game")
                        reason = str(result.get("error", "unknown quarantine reason"))[:300]
                        exclusions[reason] += 1
                        item.update(status="quarantined", reason=reason)
                    elif status == "extracted":
                        recorded = recorded_metrics(game, result, schema)
                        item.update(status="extracted", recorded=recorded)
                        for field, value in recorded["metrics"].items():
                            recorded_totals[field]["games_without_evidence" if value is None else "games_with_evidence"] += 1
                            if value is not None:
                                recorded_totals[field]["total_observed"] += value
                        if recorded["action_counts"] is not None:
                            actions.update(recorded["action_counts"])
                            action_metadata_games += 1
                        if mode == "labels":
                            audit = audit_labels(game, directory, result, recorded, schema, progress)
                            item["labels"] = audit
                            label_games += 1
                            unverified_games += not audit["hashes_verified"]
                            observed.update(audit["metrics"])
                            observed_actions.update(audit["action_counts"])
                            delays.update(audit["action_delay_frames"])
                            for phase, counts in audit["phase_action_counts"].items():
                                phase_counts[phase].update(counts)
                            by_split[game["split"]].update(audit["metrics"])
                            by_matchup[matchup(game)].update(audit["metrics"])
                    else:
                        raise ValueError("Unknown result status")
                except FileNotFoundError as error:
                    item.update(status="missing_artifact", error=Path(error.filename).name)
                except (ValueError, KeyError, TypeError, OSError, EOFError, zlib.error) as error:
                    item.update(status="invalid_artifact", error=str(error)[:500])
                statuses[item["status"]] += 1
                status_by_split[game["split"]][item["status"]] += 1
                status_by_matchup[matchup(game)][item["status"]] += 1
                if item["status"] in ("missing_artifact", "invalid_artifact") and len(errors) < 20:
                    errors.append({k: item[k] for k in ("game_id", "status", "error")})
                details.write(json.dumps(item, sort_keys=True) + "\n")
        bounded = len(selected) < len(eligible)
        scope_complete = not bounded and not statuses["missing_artifact"] and not statuses["invalid_artifact"] and not unverified_games
        report = {"version": 1, "created_at": utc_now(), "input_run": str(source), "mode": mode,
                  "cohort_sha256": cohort_digest, "schema": schema["version"], "fingerprint": schema["fingerprint"],
                  "audit_completed": True, "scope_complete": scope_complete, "source_complete": source_complete,
                  "complete": scope_complete and source_complete is True, "training_ready": False,
                  "scope": {"included_splits": sorted(splits), "eligible_games": len(eligible), "selected_games": len(selected),
                            "max_games": max_games, "bounded_pilot": bounded, "selection": "cohort order; no extrapolation",
                            "final_test_targets_opened": False, "final_test_result_files_opened": False},
                  "qualification": {"rule": cohort.get("quality_rule", "unknown"),
                                    "caveat": "Qualified ladder MMR/source claims do not verify professional identity; quality belongs to each acting perspective."},
                  "inventory": inventory, "source_status_snapshot": {"updated_at": source_status.get("updated_at"),
                                    "phase": source_status.get("phase"), **source_counts},
                  "selected_result_statuses": dict(statuses), "exclusions": dict(exclusions), "error_examples": errors,
                  "selected_statuses_by_split": dict(status_by_split), "selected_statuses_by_matchup": dict(status_by_matchup),
                  "recorded": {"metrics": recorded_totals, "action_counts": dict(actions), "games_with_action_counts": action_metadata_games,
                               "evidence": "extractor result summaries; not a fresh stream audit"},
                  "labels": {"games_audited": label_games, "games_without_verified_hashes": unverified_games,
                             "metrics": dict(observed), "action_counts": dict(observed_actions),
                             "by_split": dict(by_split), "by_matchup": dict(by_matchup),
                             "phase_action_counts": dict(phase_counts), "action_delay_frames": dict(delays)} if mode == "labels" else None,
                  "unknowns": ["No unsupported-command trace exists; wait means no accepted mapped macro command, not proven intentional waiting.",
                               "Masked windows are aggregate extractor evidence; commands without retained windows cannot all be attributed to masks.",
                               "Missing result files may be pending work or in-memory playback quarantines; they are not presumed usable.",
                               "Counts outside the selected splits/pilot and original-game fidelity are not established by this audit."]}
        accepted = observed["accepted_commands"]
        if report["labels"] is not None:
            report["labels"]["retained_positive_fraction_of_accepted_commands"] = observed["positive_samples"] / accepted if accepted else None
        write_json(output / "report.json", report)
        progress.update(force=True, stage="complete", status="complete" if report["complete"] else "partial",
                        games_processed=len(selected), scope_complete=scope_complete, source_complete=source_complete,
                        report=str(output / "report.json"))
        return report
    except Exception as error:
        progress.update(force=True, stage="failed", status="failed", error=str(error)[:500])
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", dest="input_run", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--mode", choices=("metadata", "labels"), default="metadata")
    parser.add_argument("--include-validation", action="store_true", help="Explicitly include validation metadata/labels; final test remains inaccessible")
    parser.add_argument("--max-games", type=int, help="Bound selected games in cohort order; report as a pilot when truncated")
    args = parser.parse_args()
    try:
        report = analyze(**vars(args))
    except (ValueError, OSError, KeyError, TypeError) as error:
        parser.exit(1, f"dataset analysis failed: {error}\n")
    print(json.dumps({"report": str(args.output / "report.json"), "complete": report["complete"],
                      "scope_complete": report["scope_complete"], "selected_result_statuses": report["selected_result_statuses"]}))


if __name__ == "__main__":
    main()
