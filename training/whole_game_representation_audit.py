"""Bounded, split-safe audit of causal cadence and multi-slot label representation."""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import statistics

from .whole_game_cadence_sequences import cadence_sequences, slot_targets
from .whole_game_shards import selected_shards, trajectory_shard, load_terrain


SCHEMA = "protodd-whole-game-representation-audit-v1"
MATCHUPS = ("PvP", "PvT", "PvZ")
DELAY_BINS = ((0, 1, "0-1"), (2, 5, "2-5"), (6, 11, "6-11"), (12, 23, "12-23"))


def _counter(counter):
    return {str(key): counter[key] for key in sorted(counter, key=str)}


def _percentile(values, fraction):
    ordered = sorted(values)
    if not ordered:
        return None
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


class _Bucket:
    def __init__(self):
        self.windows = 0
        self.commands = 0
        self.overflow_commands = 0
        self.repeated_actor_commands = 0
        self.repeated_actor_windows = 0
        self.command_counts = Counter()
        self.kinds = Counter()
        self.target_modes = Counter()
        self.actor_group_sizes = Counter()
        self.actor_availability = Counter()
        self.actor_ids_missing_from_observation = 0
        self.target_entity_availability = Counter()
        self.active_slots = Counter()
        self.stop_slots = Counter()
        self.delay_frames = []
        self.delay_bins = Counter()
        self.position_labels = 0
        self.position_in_bounds = 0
        self.position_out_of_bounds = 0
        self.position_by_kind = Counter()
        self.position_by_coordinate_space = Counter()

    def add_sequence(self, sequence, *, maximum_slots, window, map_width_px,
                     map_height_px):
        labels = sequence["labels"]
        event = bool(sequence["event"])
        if event != bool(labels):
            raise ValueError("cadence event and commands disagree")
        self.windows += 1
        self.command_counts[len(labels)] += 1
        if len(labels) > maximum_slots:
            self.overflow_commands += len(labels) - maximum_slots
        slots = slot_targets(sequence, maximum_commands=maximum_slots, window=window)
        for ordinal, slot in enumerate(slots["slots"]):
            if slot["stop"]:
                self.stop_slots[ordinal] += 1
            else:
                self.active_slots[ordinal] += 1

        observation = sequence["observation"]
        own_ids = {entity["id"] for entity in observation["entities"]
                   if entity["relation"] == 0}
        visible_ids = {entity["id"] for entity in observation["entities"]
                       if entity["visible"]}
        actor_occurrences = Counter()
        for index, label in enumerate(labels):
            actions, masks = label["actions"], label["loss_masks"]
            kind = actions["kind"]
            self.commands += 1
            self.kinds[kind] += 1
            actor_ids = set(label["actor_positive"])
            self.actor_group_sizes[len(actor_ids)] += 1
            expected_actor_available = bool(actor_ids) and actor_ids <= own_ids
            if bool(sequence["actor_available"][index]) != expected_actor_available:
                raise ValueError("actor availability does not match the causal observation")
            self.actor_availability["available" if expected_actor_available else "unavailable"] += 1
            missing_actors = actor_ids - own_ids
            self.actor_ids_missing_from_observation += len(missing_actors)
            actor_occurrences.update(actor_ids)

            if masks.get("target_mode"):
                self.target_modes[actions["target_mode"]] += 1
            if masks.get("target_entity"):
                expected_target_available = actions["target_entity"] in visible_ids
                if bool(sequence["target_available"][index]) != expected_target_available:
                    raise ValueError("target availability does not match causal visibility")
                self.target_entity_availability[
                    "available" if expected_target_available else "unavailable"] += 1
            elif not sequence["target_available"][index]:
                raise ValueError("target without an entity target was marked unavailable")

            delay = label["frame"] - observation["frame"]
            if not 0 <= delay < window:
                raise ValueError("command delay lies outside its cadence window")
            self.delay_frames.append(delay)
            for lower, upper, name in DELAY_BINS:
                if lower <= delay <= upper:
                    self.delay_bins[name] += 1
                    break

            if masks.get("target_position"):
                coordinate_space = label["coordinate_space"]
                factor = {"pixel": 1, "build_tile": 32}.get(coordinate_space)
                if factor is None:
                    raise ValueError("unknown target position coordinate space")
                x, y = actions["target_position"]
                x, y = x * factor, y * factor
                self.position_labels += 1
                self.position_by_kind[kind] += 1
                self.position_by_coordinate_space[coordinate_space] += 1
                if 0 <= x < map_width_px and 0 <= y < map_height_px:
                    self.position_in_bounds += 1
                else:
                    self.position_out_of_bounds += 1
        repeated = sum(max(0, count - 1) for count in actor_occurrences.values())
        self.repeated_actor_commands += repeated
        self.repeated_actor_windows += repeated > 0

    def merge(self, other):
        for field in ("windows", "commands", "overflow_commands",
                      "repeated_actor_commands", "repeated_actor_windows",
                      "actor_ids_missing_from_observation", "position_labels",
                      "position_in_bounds", "position_out_of_bounds"):
            setattr(self, field, getattr(self, field) + getattr(other, field))
        for field in ("command_counts", "kinds", "target_modes", "actor_group_sizes",
                      "actor_availability", "target_entity_availability", "active_slots",
                      "stop_slots", "delay_bins", "position_by_kind",
                      "position_by_coordinate_space"):
            getattr(self, field).update(getattr(other, field))
        self.delay_frames.extend(other.delay_frames)

    def report(self):
        active_count, stop_count = sum(self.active_slots.values()), sum(self.stop_slots.values())
        return {
            "complete_windows": self.windows,
            "commands": self.commands,
            "command_count_per_window": _counter(self.command_counts),
            "command_kinds": _counter(self.kinds),
            "target_modes_with_known_labels": _counter(self.target_modes),
            "active_slots_by_ordinal": _counter(self.active_slots),
            "stop_slots_by_ordinal": _counter(self.stop_slots),
            "stop_to_active_slot_ratio": stop_count / active_count if active_count else None,
            "overflow_commands_beyond_slot_limit": self.overflow_commands,
            "actor_group_size": _counter(self.actor_group_sizes),
            "actor_set_availability": _counter(self.actor_availability),
            "actor_ids_absent_from_causal_observation": self.actor_ids_missing_from_observation,
            "repeated_actor_command_labels": self.repeated_actor_commands,
            "windows_with_repeated_actor_labels": self.repeated_actor_windows,
            "entity_target_availability": _counter(self.target_entity_availability),
            "delay_frames": {
                "count": len(self.delay_frames),
                "minimum": min(self.delay_frames) if self.delay_frames else None,
                "median": statistics.median(self.delay_frames) if self.delay_frames else None,
                "p90": _percentile(self.delay_frames, 0.90),
                "maximum": max(self.delay_frames) if self.delay_frames else None,
                "bins": _counter(self.delay_bins),
            },
            "position_labels": {
                "count": self.position_labels,
                "in_map_bounds": self.position_in_bounds,
                "out_of_map_bounds": self.position_out_of_bounds,
                "by_kind": _counter(self.position_by_kind),
                "coordinate_space": _counter(self.position_by_coordinate_space),
            },
        }


class _Summary:
    def __init__(self):
        self.cadence_observations = 0
        self.censored_windows_within_limit = 0
        self.censored_final_windows_full_game = 0
        self.all = _Bucket()
        self.by_event = {"action": _Bucket(), "no_action": _Bucket()}

    def observe_cadence(self, censored, within_frame_limit):
        if censored:
            self.censored_final_windows_full_game += 1
        if within_frame_limit:
            self.cadence_observations += 1
            self.censored_windows_within_limit += bool(censored)

    def add_sequence(self, sequence, **kwargs):
        event = "action" if sequence["event"] else "no_action"
        self.all.add_sequence(sequence, **kwargs)
        self.by_event[event].add_sequence(sequence, **kwargs)

    def merge(self, other):
        self.cadence_observations += other.cadence_observations
        self.censored_windows_within_limit += other.censored_windows_within_limit
        self.censored_final_windows_full_game += other.censored_final_windows_full_game
        self.all.merge(other.all)
        for event in self.by_event:
            self.by_event[event].merge(other.by_event[event])

    def report(self):
        complete = self.all.windows
        event_windows = self.by_event["action"].windows
        return {
            "cadence_observations_within_frame_limit": self.cadence_observations,
            "complete_windows_within_frame_limit": complete,
            "censored_windows_within_frame_limit": self.censored_windows_within_limit,
            "censored_final_windows_full_game": self.censored_final_windows_full_game,
            "event_conditioned_windows": {
                "action": event_windows,
                "no_action": self.by_event["no_action"].windows,
                "action_rate_among_complete": event_windows / complete if complete else None,
            },
            "all_complete_windows": self.all.report(),
            "by_event": {event: bucket.report() for event, bucket in self.by_event.items()},
        }


def _identity(release):
    release = Path(release)
    data = json.loads((release / "release.json").read_text(encoding="utf-8"))
    identity_path = release / "identity.json"
    identity = json.loads(identity_path.read_text(encoding="utf-8"))
    if data.get("complete") is not True:
        raise ValueError(f"incomplete replay release: {release}")
    return release, data, identity, hashlib.sha256(identity_path.read_bytes()).hexdigest()


def _cohort(identity, split, games_per_matchup):
    groups = {matchup: [] for matchup in MATCHUPS}
    for record in identity["selected"]:
        if record["split"] == split and record["matchup"] in groups:
            groups[record["matchup"]].append(record["game_id"])
    result = {}
    for matchup, game_ids in groups.items():
        chosen = sorted(set(game_ids))[:games_per_matchup]
        if len(chosen) != games_per_matchup:
            raise ValueError(f"not enough {split} games for {matchup}")
        result[matchup] = chosen
    return result


def _validate_split_identity(train_identity, validation_identity):
    train_rows = [row for row in train_identity["selected"] if row["split"] == "train"]
    validation_rows = [row for row in validation_identity["selected"]
                       if row["split"] == "validation"]
    train_ids = {row["game_id"] for row in train_rows}
    validation_ids = {row["game_id"] for row in validation_rows}
    train_replays = {row["replay_sha256"] for row in train_rows}
    validation_replays = {row["replay_sha256"] for row in validation_rows}
    if not train_ids or not validation_ids or train_ids & validation_ids:
        raise ValueError("training and validation identities overlap or are empty")
    if train_replays & validation_replays:
        raise ValueError("training and validation replay assets overlap")


def _audit_split(release, split, cohort, *, maximum_frame, window, maximum_slots):
    selected_ids = {game_id for game_ids in cohort.values() for game_id in game_ids}
    overall = _Summary()
    by_matchup = {matchup: _Summary() for matchup in MATCHUPS}
    by_game = []
    found = set()
    for record, directory, _ in selected_shards(
            release, split, require_complete=True, game_ids=selected_ids):
        found.add(record["game_id"])
        terrain = load_terrain(directory)
        map_width_px = terrain["width_walktiles"] * 8
        map_height_px = terrain["height_walktiles"] * 8
        game = _Summary()

        def counted_trajectory():
            for observation, target in trajectory_shard(directory, window=window):
                if target["update_memory"]:
                    game.observe_cadence(target["event"] is None,
                                         observation["frame"] <= maximum_frame)
                yield observation, target

        for sequence in cadence_sequences(counted_trajectory(), window=window):
            if sequence["observation"]["frame"] > maximum_frame:
                continue
            game.add_sequence(sequence, maximum_slots=maximum_slots, window=window,
                              map_width_px=map_width_px, map_height_px=map_height_px)
        if (game.cadence_observations !=
                game.all.windows + game.censored_windows_within_limit):
            raise ValueError("complete and censored cadence windows do not reconcile")
        overall.merge(game)
        by_matchup[record["matchup"]].merge(game)
        by_game.append({"game_id": record["game_id"], "matchup": record["matchup"],
                        "cadence": game.report()})
    if found != selected_ids:
        raise ValueError(f"selected {split} cohort is incomplete: {sorted(selected_ids - found)}")
    return {"cohort_game_ids": cohort, "overall": overall.report(),
            "by_matchup": {matchup: summary.report()
                           for matchup, summary in by_matchup.items()},
            "by_game": by_game}


def audit(train_release, validation_release, *, games_per_matchup=2,
          maximum_frame=3600, window=24, maximum_slots=6):
    if (games_per_matchup < 1 or maximum_frame < window or window < 1 or
            maximum_slots < 1):
        raise ValueError("invalid bounded representation-audit limits")
    train_release, train_info, train_identity, train_sha = _identity(train_release)
    validation_release, validation_info, validation_identity, validation_sha = _identity(
        validation_release)
    _validate_split_identity(train_identity, validation_identity)

    train_cohort = _cohort(train_identity, "train", games_per_matchup)
    validation_cohort = _cohort(validation_identity, "validation", games_per_matchup)
    train = _audit_split(train_release, "train", train_cohort,
                         maximum_frame=maximum_frame, window=window,
                         maximum_slots=maximum_slots)
    validation = _audit_split(validation_release, "validation", validation_cohort,
                              maximum_frame=maximum_frame, window=window,
                              maximum_slots=maximum_slots)
    source_files = (Path(__file__), Path(__file__).with_name("whole_game_cadence_sequences.py"),
                    Path(__file__).with_name("whole_game_shards.py"))
    return {
        "schema": SCHEMA,
        "predeclared_limits": {"games_per_matchup": games_per_matchup,
                               "maximum_frame": maximum_frame, "window_frames": window,
                               "maximum_slots": maximum_slots,
                               "cohort_selection": "lexicographically first game IDs within each split/matchup"},
        "metric_definitions": {
            "action_window": "at least one confirmed action in the next window_frames",
            "censored_window": "cadence observation whose timing event has no complete future window",
            "actor_available": "all replay-positive actor IDs were owned at the cadence observation",
            "entity_target_available": "a required entity target was visible at the cadence observation",
            "stop_slot": "explicit STOP target emitted only when fewer than maximum_slots commands are present",
            "position_in_bounds": "replay position, converted to pixels, lies inside the map rectangle",
            "delay_bins": ["0-1", "2-5", "6-11", "12-23"],
        },
        "safety": {"final_test_read": False, "optimizer_run": False,
                   "train_validation_game_overlap": False,
                   "train_validation_replay_overlap": False,
                   "validation_labels_modified": False},
        "releases": {
            "train": {"path": str(train_release.resolve()), "identity_sha256": train_sha,
                      "release_training_ready": train_info.get("training_ready", False)},
            "validation": {"path": str(validation_release.resolve()),
                           "identity_sha256": validation_sha,
                           "release_training_ready": validation_info.get("training_ready", False)},
        },
        "source_sha256": {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                           for path in source_files},
        "train": train,
        "validation": validation,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("train_release", type=Path)
    parser.add_argument("validation_release", type=Path)
    parser.add_argument("--games-per-matchup", type=int, default=2)
    parser.add_argument("--maximum-frame", type=int, default=3600)
    parser.add_argument("--window", type=int, default=24)
    parser.add_argument("--maximum-slots", type=int, default=6)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    report = audit(args.train_release, args.validation_release,
                   games_per_matchup=args.games_per_matchup,
                   maximum_frame=args.maximum_frame, window=args.window,
                   maximum_slots=args.maximum_slots)
    rendered = json.dumps(report, indent=2, sort_keys=True) + "\n"
    if args.output:
        if args.output.exists():
            raise FileExistsError(args.output)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(rendered, encoding="utf-8")
    print(json.dumps({"schema": report["schema"],
                      "train_windows": report["train"]["overall"]["complete_windows_within_frame_limit"],
                      "validation_windows": report["validation"]["overall"]["complete_windows_within_frame_limit"],
                      "output": str(args.output.resolve()) if args.output else None}, indent=2))


if __name__ == "__main__":
    main()
