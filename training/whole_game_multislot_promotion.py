"""Promote a six-slot policy only from matching replay, Win32 and game evidence.

An evaluation build may play local games before this receipt exists. The normal
control build rechecks the frozen evidence at CMake configure time.
"""
from __future__ import annotations

import argparse
from collections import Counter
import hashlib
import json
import math
from pathlib import Path
import re
import struct
import subprocess

from . import arena
from .whole_game_controller_audit import summarize as controller_audit


SCHEMA = "protodd-whole-game-promotion-v1"
MATCHUPS = {"PvP", "PvT", "PvZ"}
EVIDENCE_NAMES = ("package_manifest", "checkpoint", "run", "training_identity",
                  "validation_identity", "audit", "parity_early", "parity_mid",
                  "parity_late", "benchmark", "candidate_dll", "evaluation_build_record",
                  "resource_probe",
                  "candidate_manifest", "candidate_results", "baseline_manifest",
                  "baseline_results")


def digest(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def source_fingerprint():
    """Fingerprint sources that can change the compiled tournament DLL."""
    root = Path(__file__).resolve().parents[1]
    files = [root / "CMakeLists.txt", *sorted((root / "src").rglob("*.cpp")),
             *sorted((root / "src").rglob("*.hpp")),
             *sorted((root / "include").rglob("*.hpp")),
             *sorted((root / "cmake").rglob("*.cmake"))]
    combined = hashlib.sha256()
    for path in files:
        combined.update(path.relative_to(root).as_posix().encode() + b"\0")
        combined.update(bytes.fromhex(digest(path)))
    return combined.hexdigest()


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf8"))


def require_win32_pe(executable):
    with Path(executable).open("rb") as stream:
        if stream.read(2) != b"MZ":
            raise ValueError("promotion probe is not a Windows executable")
        stream.seek(0x3c)
        offset = struct.unpack("<I", stream.read(4))[0]
        stream.seek(offset)
        if stream.read(4) != b"PE\0\0" or struct.unpack("<H", stream.read(2))[0] != 0x14c:
            raise ValueError("promotion probe must be Win32 x86")


def require(condition, message):
    if not condition:
        raise ValueError(message)


def candidate_traces(campaign, games):
    """Freeze live logs and reject the repeated early worker move failure."""
    combined = hashlib.sha256()
    for game in games:
        received = (Path(campaign) / "server/replays/bot-write" /
                    f"game-{game['gameID']}" / "Protodd/received")
        targets = {}
        for name in ("WholeGame-controller.txt", "WholeGame-inference.csv",
                     "WholeGame-intents.csv", "Protodd.log"):
            path = received / name
            require(path.is_file(), f"missing live controller trace: {path}")
            combined.update(str(game["gameID"]).encode() + b":" + name.encode() + b"\0")
            combined.update(bytes.fromhex(digest(path)))
        with (received / "Protodd.log").open(encoding="utf8", errors="replace") as stream:
            for line in stream:
                parts = line.strip().split(",")
                if (len(parts) < 8 or parts[0] != "ACTION" or
                        parts[3] != "whole-game" or
                        parts[4] not in {"Move", "Right_Click_Position",
                                         "Attack_Move", "Patrol"} or
                        parts[5:7] != ["issued", "accepted"]):
                    continue
                try:
                    frame, actor = int(parts[1]), int(parts[2])
                except ValueError:
                    continue
                if frame >= 1200:
                    continue
                fields = dict(item.split("=", 1) for item in parts[7:] if "=" in item)
                if not re.fullmatch(r"\d+", fields.get("x", "")) or not re.fullmatch(
                        r"\d+", fields.get("y", "")):
                    continue
                key = (int(fields["x"]), int(fields["y"]))
                entry = targets.setdefault(key, (set(), set()))
                entry[0].add(actor)
                entry[1].add(frame)
        require(not any(len(actors) >= 4 and len(frames) >= 3
                        for actors, frames in targets.values()),
                f"repeated early mass moves detected in game {game['gameID']}")
    return combined.hexdigest()


def evidence_paths(package, checkpoint, training_release, validation_release,
                   audit, parity_early, parity_mid, parity_late, benchmark,
                   candidate, baseline, dll, evaluation_build_record, resource_probe):
    return dict(package_manifest=Path(package) / "manifest.json",
                checkpoint=Path(checkpoint), run=Path(checkpoint).parent / "run.json",
                training_identity=Path(training_release) / "identity.json",
                validation_identity=Path(validation_release) / "identity.json",
                audit=Path(audit), parity_early=Path(parity_early),
                parity_mid=Path(parity_mid), parity_late=Path(parity_late),
                benchmark=Path(benchmark), candidate_dll=Path(dll),
                evaluation_build_record=Path(evaluation_build_record),
                resource_probe=Path(resource_probe),
                candidate_manifest=Path(candidate) / "manifest.json",
                candidate_results=Path(candidate) / "server/results.jsonl",
                baseline_manifest=Path(baseline) / "manifest.json",
                baseline_results=Path(baseline) / "server/results.jsonl")


def evaluate(package, checkpoint, training_release, validation_release, audit,
             parity_early, parity_mid, parity_late, benchmark, candidate, baseline,
             dll, evaluation_build_record, resource_probe, parity_probe, *, minimum_games=72):
    paths = evidence_paths(package, checkpoint, training_release, validation_release,
                           audit, parity_early, parity_mid, parity_late, benchmark,
                           candidate, baseline, dll, evaluation_build_record,
                           resource_probe)
    require(all(path.is_file() for path in paths.values()), "promotion evidence file missing")
    package = Path(package)
    weights = package / "weights.bin"
    require(weights.is_file(), "packaged weights missing")
    manifest = read_json(paths["package_manifest"])
    require(manifest.get("schema") == "protodd-whole-game-multislot-weights-v2" and
            manifest.get("maximum_slots") == 6 and
            manifest.get("tournament_ready") is False, "not an unpromoted six-slot package")
    weights_sha = digest(weights)
    require(manifest.get("weights_sha256") == weights_sha and
            manifest.get("checkpoint_sha256") == digest(checkpoint) and
            manifest.get("run_sha256") == digest(paths["run"]),
            "weight package, checkpoint or frozen run changed")
    with weights.open("rb") as stream:
        header = stream.read(20)
    require(len(header) == 20 and header[:8] == b"PWGM1\0\0\0" and
            struct.unpack("<I", header[8:12])[0] == 2,
            "embedded package is not the six-slot binary format")
    run = read_json(paths["run"])
    require(run.get("schema") == "protodd-whole-game-multislot-fit-v1" and
            run.get("maximum_slots") == 6 and
            run.get("training_identity_sha256") == manifest.get("source_identity_sha256"),
            "frozen run does not match exported model")

    train_sha = digest(paths["training_identity"])
    validation_sha = digest(paths["validation_identity"])
    train = read_json(paths["training_identity"])
    validation = read_json(paths["validation_identity"])
    train_rows = [row for row in train["selected"] if row["split"] == "train"]
    validation_rows = [row for row in validation["selected"]
                       if row["split"] == "validation"]
    require(train_rows and validation_rows and
            not ({row["game_id"] for row in train_rows} &
                 {row["game_id"] for row in validation_rows}) and
            not ({row["replay_sha256"] for row in train_rows} &
                 {row["replay_sha256"] for row in validation_rows}) and
            manifest["source_identity_sha256"] == train_sha,
            "training and validation replays are not disjoint or model source changed")

    replay = read_json(audit)
    audit_sources = replay.get("source_code_sha256", {})
    require(audit_sources and all(
        digest(Path(__file__).parent / name) == sha
        for name, sha in audit_sources.items()),
        "causal audit source changed since the report was produced")
    fit_sources = run.get("source_code_sha256", {})
    require(fit_sources and set(fit_sources) <= set(audit_sources) and
            "whole_game_multislot_audit.py" in audit_sources and
            all(audit_sources[name] == sha for name, sha in fit_sources.items()),
        "audit model or encoder source differs from the frozen fit")
    require(replay.get("schema") == "protodd-whole-game-multislot-audit-v1" and
            replay.get("teacher_sha256") == manifest["checkpoint_sha256"] and
            replay.get("training_identity_sha256") == train_sha and
            replay.get("validation_identity_sha256") == validation_sha and
            all(replay.get("games", {}).get(matchup, 0) >= 8 for matchup in MATCHUPS),
            "causal replay audit is missing, shallow or belongs to another model")
    rows = replay.get("rows", [])
    require(all(len({row["game_id"] for row in rows if row["matchup"] == matchup}) >= 8
                for matchup in MATCHUPS), "audit does not cover eight distinct games per matchup")
    overall = replay["overall"]
    require(overall.get("windows") == len(rows) and overall.get("replay_commands", 0) >= 100 and
            overall.get("early_mass_probe_move_patterns") == 0 and
            overall.get("predicted_commands", 0) >= 100 and
            overall.get("first_actor_correct", 0) >= 0.05 * overall["action_windows"] and
            overall.get("non_right_click_correct", 0) >=
            0.05 * overall["non_right_click_commands"] and
            overall.get("full_signature_correct", 0) >=
            0.01 * overall["slot_commands"] and
            overall.get("position_known", 0) >= 10 and
            overall.get("position_within_64px", 0) >=
            0.02 * overall["position_known"],
            "causal replay command quality is below the minimum promotion floor")
    for kind in ("build", "train", "right_click"):
        entry = overall.get("kind_by_name", {}).get(kind, {})
        require(entry.get("samples", 0) >= 10 and entry.get("correct", 0) > 0,
                f"replay audit has no useful {kind} coverage")
    combat = [overall.get("kind_by_name", {}).get(kind, {})
              for kind in ("attack", "attack_move")]
    require(sum(row.get("samples", 0) for row in combat) >= 10 and
            sum(row.get("correct", 0) for row in combat) > 0,
            "replay audit has no useful combat-command coverage")

    parity_probe = Path(parity_probe)
    require(parity_probe.is_file(), "Win32 parity executable missing")
    require_win32_pe(parity_probe)
    parity_probe_sha = digest(parity_probe)
    parity_summary = {}
    for stage, path, frame in (("early", parity_early, 0),
                               ("mid", parity_mid, 3000),
                               ("late", parity_late, 9000)):
        report = read_json(path)
        cases = report.get("cases", [])
        require(report.get("schema") == "protodd-whole-game-multislot-cpu-parity-v1" and
                report.get("passed") is True and
                report.get("architecture") == "Win32-x86" and
                report.get("executable_sha256") == parity_probe_sha and
                report.get("package_sha256") == weights_sha and
                report.get("checkpoint_sha256") == manifest["checkpoint_sha256"] and
                cases and min(case["frame"] for case in cases) >= frame and
                (stage != "early" or max(case["frame"] for case in cases) < 1000) and
                (stage != "mid" or max(case["frame"] for case in cases) < 9000) and
                report.get("max_abs_error", math.inf) <= 3e-4 and
                all(case["max_abs_error"] <= 3e-4 and
                    case.get("compiled_ms") is not None and
                    case["compiled_ms"] < 35 for case in cases),
                f"{stage} Win32 numerical or latency parity failed")
        if stage == "mid":
            require({case["matchup"] for case in cases} == MATCHUPS,
                    "midgame parity does not cover PvP, PvT and PvZ")
        parity_summary[stage] = dict(cases=len(cases), max_abs_error=report["max_abs_error"],
                                     maximum_ms=max(case["compiled_ms"] for case in cases))

    timing = read_json(benchmark)
    require(timing.get("schema") == "protodd-whole-game-win32-forward-benchmark-v1" and
            timing.get("architecture") == "Win32" and
            timing.get("weights_sha256") == weights_sha and
            timing.get("probe_sha256") == parity_probe_sha and
            timing.get("entities", 0) >= 100 and
            timing.get("active_slots", 0) >= 4 and
            len(timing.get("samples_ms", [])) >= 20 and
            timing.get("p95_ms", math.inf) < 30 and
            timing.get("max_ms", math.inf) < 35,
            "late-game Win32 benchmark is missing or exceeds the frame budget")

    resource_probe = Path(resource_probe)
    require_win32_pe(resource_probe)
    build_record = read_json(evaluation_build_record)
    require(build_record.get("schema") == "protodd-whole-game-evaluation-build-v1" and
            build_record.get("dll_sha256") == digest(dll) and
            build_record.get("source_fingerprint") == source_fingerprint(),
            "evaluation DLL was not built from the current frozen source")
    process = subprocess.run([str(resource_probe), str(dll), str(weights)],
                             capture_output=True, text=True, timeout=120)
    require(process.returncode == 0 and "max_abs_difference=0" in process.stdout,
            "candidate DLL does not embed exactly the evaluated weights: " + process.stderr[:300])
    candidate, baseline = Path(candidate), Path(baseline)
    arena.verify(candidate)
    arena.verify(baseline)
    candidate_manifest = read_json(paths["candidate_manifest"])
    baseline_manifest = read_json(paths["baseline_manifest"])
    require(candidate_manifest.get("race") == baseline_manifest.get("race") == "Protoss" and
            candidate_manifest.get("bot") == baseline_manifest.get("bot") == "Protodd" and
            candidate_manifest.get("purpose") in ("development", "final-test") and
            baseline_manifest.get("purpose") in ("development", "final-test") and
            candidate_manifest["components"].get("server/bots/Protodd/AI/Protodd.dll") == digest(dll),
            "candidate campaign was not played by the supplied evaluation DLL")
    candidate_components = candidate_manifest["components"]
    baseline_components = baseline_manifest["components"]
    common = [name for name in candidate_components if name.startswith("server/bots/") and
              "/AI/" in name and "/Protodd/" not in name]
    require(common and all(candidate_components[name] == baseline_components.get(name)
                           for name in common) and
            candidate_components.get("server/server.jar") ==
            baseline_components.get("server/server.jar"),
            "opponent DLLs or manager changed between paired campaigns")
    schedule = [json.loads(line) for line in
                (candidate / "server/games.jsonl").read_text(encoding="utf8").splitlines()]
    baseline_schedule = [json.loads(line) for line in
                         (baseline / "server/games.jsonl").read_text(encoding="utf8").splitlines()]
    require(schedule == baseline_schedule and len(schedule) >= minimum_games and
            len({game["map"] for game in schedule}) >= 3 and
            len({game["homeBot"] if game["homeBot"] != "Protodd" else game["awayBot"]
                 for game in schedule}) >= 3,
            "campaigns need the same schedule and at least 72 diverse games")
    opponents = {game["homeBot"] if game["homeBot"] != "Protodd" else game["awayBot"]
                 for game in schedule}
    settings = read_json(candidate / "server/server_settings.json")
    races = {entry["BotName"]: entry["Race"] for entry in settings["bots"]}
    require({races[name] for name in opponents} == {"Protoss", "Terran", "Zerg"} and
            {game["homeBot"] for game in schedule} >= {"Protodd"} | opponents and
            {game["awayBot"] for game in schedule} >= {"Protodd"} | opponents,
            "paired campaigns need opponents of all three races on both host sides")
    operational = controller_audit(candidate)
    require(operational["controller_operational"] and
            operational["scheduled"] == len(schedule),
            "candidate controller did not finish every game within runtime limits")
    trace_sha = candidate_traces(candidate, schedule)
    candidate_games = arena.inspect(candidate)
    baseline_games = arena.inspect(baseline)
    require(not candidate_games["excluded"] and not baseline_games["excluded"] and
            len(candidate_games["structurally_valid"]) == len(schedule) and
            len(baseline_games["structurally_valid"]) == len(schedule),
            "campaign outcomes are incomplete or not healthy paired reports")
    candidate_wins = {row["game_id"]: row["won"] for row in
                      candidate_games["structurally_valid"]}
    baseline_wins = {row["game_id"]: row["won"] for row in
                     baseline_games["structurally_valid"]}
    by_race = {race: Counter() for race in ("Protoss", "Terran", "Zerg")}
    for game in schedule:
        opponent = game["homeBot"] if game["homeBot"] != "Protodd" else game["awayBot"]
        counts = by_race[races[opponent]]
        counts["games"] += 1
        counts["candidate_wins"] += candidate_wins[game["gameID"]]
        counts["baseline_wins"] += baseline_wins[game["gameID"]]
    total_candidate = sum(candidate_wins.values())
    total_baseline = sum(baseline_wins.values())
    require(total_candidate - total_baseline >= max(4, math.ceil(len(schedule) * 0.05)) and
            all(counts["games"] >= 16 and
                counts["candidate_wins"] >= counts["baseline_wins"] - 1
                for counts in by_race.values()),
            "candidate did not improve on the established bot without a severe matchup regression")
    return dict(weights_sha256=weights_sha, checkpoint_sha256=digest(checkpoint),
                source_fingerprint=source_fingerprint(),
                candidate_dll_sha256=digest(dll), games=len(schedule),
                candidate_trace_sha256=trace_sha,
                candidate_wins=total_candidate, baseline_wins=total_baseline,
                by_race={race: dict(counts) for race, counts in by_race.items()},
                parity=parity_summary, replay_audit=dict(
                    windows=overall["windows"],
                    full_signature_correct=overall["full_signature_correct"],
                    non_right_click_correct=overall["non_right_click_correct"]))


def promote(args):
    output = Path(args.output)
    require(not output.exists(), "promotion receipt already exists")
    kwargs = dict(package=args.package, checkpoint=args.checkpoint,
                  training_release=args.training_release,
                  validation_release=args.validation_release, audit=args.audit,
                  parity_early=args.parity_early, parity_mid=args.parity_mid,
                  parity_late=args.parity_late, benchmark=args.benchmark,
                  candidate=args.candidate, baseline=args.baseline, dll=args.dll,
                  evaluation_build_record=args.evaluation_build_record,
                  resource_probe=args.resource_probe, parity_probe=args.parity_probe)
    summary = evaluate(**kwargs)
    paths = evidence_paths(*(kwargs[key] for key in (
        "package", "checkpoint", "training_release", "validation_release", "audit",
        "parity_early", "parity_mid", "parity_late", "benchmark", "candidate",
        "baseline", "dll", "evaluation_build_record", "resource_probe")))
    receipt = dict(schema=SCHEMA, tournament_ready=True,
                   weights_sha256=summary["weights_sha256"], summary=summary,
                   inputs={key: str(Path(value).resolve()) for key, value in kwargs.items()},
                   evidence={name: dict(path=str(path.resolve()), sha256=digest(path))
                             for name, path in paths.items()})
    receipt["evidence"]["weights"] = dict(
        path=str((Path(args.package) / "weights.bin").resolve()),
        sha256=summary["weights_sha256"])
    receipt["evidence"]["parity_probe"] = dict(
        path=str(Path(args.parity_probe).resolve()), sha256=digest(args.parity_probe))
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf8")
    return receipt


def verify(receipt_path, weights):
    receipt = read_json(receipt_path)
    require(receipt.get("schema") == SCHEMA and receipt.get("tournament_ready") is True and
            receipt.get("weights_sha256") == digest(weights),
            "receipt is not ready for these embedded weights")
    evidence = receipt.get("evidence", {})
    require(set(evidence) == set(EVIDENCE_NAMES) | {"weights", "parity_probe"},
            "promotion evidence list is incomplete")
    for name, item in evidence.items():
        require(digest(item["path"]) == item["sha256"],
                f"promotion evidence changed: {name}")
    require(Path(evidence["weights"]["path"]).resolve() == Path(weights).resolve(),
            "receipt refers to another weight file")
    inputs = receipt["inputs"]
    require(set(inputs) == {"package", "checkpoint", "training_release",
                            "validation_release", "audit", "parity_early", "parity_mid",
                            "parity_late", "benchmark", "candidate", "baseline", "dll",
                            "evaluation_build_record", "resource_probe", "parity_probe"},
            "promotion input list changed")
    summary = evaluate(**inputs)
    require(summary == receipt["summary"], "promotion evidence summary changed")
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("promote")
    record = commands.add_parser("record-evaluation")
    record.add_argument("dll", type=Path)
    record.add_argument("output", type=Path)
    for name in ("package", "checkpoint", "training_release", "validation_release",
                 "audit", "parity_early", "parity_mid", "parity_late", "benchmark",
                 "candidate", "baseline", "dll", "resource_probe", "parity_probe",
                 "evaluation_build_record", "output"):
        create.add_argument("--" + name.replace("_", "-"), type=Path, required=True)
    check = commands.add_parser("verify")
    check.add_argument("receipt", type=Path)
    check.add_argument("weights", type=Path)
    args = parser.parse_args()
    if args.command == "promote":
        result = promote(args)
    elif args.command == "record-evaluation":
        require(args.dll.name == "ProtoddEvaluation.dll", "expected evaluation DLL")
        require_win32_pe(args.dll)
        result = dict(schema="protodd-whole-game-evaluation-build-v1",
                      dll_sha256=digest(args.dll),
                      source_fingerprint=source_fingerprint())
        args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf8")
    else:
        result = verify(args.receipt, args.weights)
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
