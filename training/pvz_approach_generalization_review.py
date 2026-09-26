"""Read-only fresh-seed PvZ comparison with an exact same-DLL repeat arm."""

import argparse
from collections import defaultdict
import hashlib
import json
from pathlib import Path

from .arena import verify
from .pvz_approach_grace_screen import delayed_pylons
from .pvz_core_transition_review import trace
from .schema import sha256


def campaign(path, games, frame_limit):
    verify(path)
    manifest = json.loads((path / "manifest.json").read_text())
    schedule = {row["gameID"]: row for row in map(
        json.loads, (path / "server/games.jsonl").read_text().splitlines())}
    grouped = defaultdict(list)
    for line in (path / "server/results.jsonl").read_text().splitlines():
        result = json.loads(line)
        grouped[result["gameID"]].append(result)
    if manifest["games"] != games or len(schedule) != games or set(grouped) != set(schedule):
        raise ValueError(f"missing or extra games: {path}")
    rows = []
    for game_id, game in sorted(schedule.items()):
        pair = grouped[game_id]
        if len(pair) != 2 or {r["reportingBot"] for r in pair} != \
                {game["homeBot"], game["awayBot"]}:
            raise ValueError(f"missing result pair: {path} game {game_id}")
        for result in pair:
            if result["opponentBot"] not in (game["homeBot"], game["awayBot"]) or \
                    result["opponentBot"] == result["reportingBot"] or \
                    result["map"] != game["map"] or \
                    result["wasHost"] != (result["reportingBot"] == game["homeBot"]) or \
                    result["gameEndType"] != "NORMAL" or result["crash"] or \
                    result["gameTimeout"] or type(result["won"]) is not bool or \
                    result["finalFrame"] > frame_limit + 2:
                raise ValueError(f"unhealthy pair: {path} game {game_id}")
        capped = all(not r["won"] and r["finalFrame"] == frame_limit + 2 for r in pair)
        decisive = (pair[0]["won"] != pair[1]["won"] and
                    abs(pair[0]["finalFrame"] - pair[1]["finalFrame"]) <= 120)
        if not (capped or decisive):
            raise ValueError(f"ambiguous result: {path} game {game_id}")
        own = next(r for r in pair if r["reportingBot"] == "Protodd")
        log = path / f"server/replays/bot-write/game-{game_id}/Protodd/received/Protodd.log"
        item = trace(log)
        if item["match"] is None or item["errors"] or not item["enemy_activity"] or \
                not all(str(frame) in item["states"] for frame in (6000, 8400)
                        if own["finalFrame"] >= frame):
            raise ValueError(f"missing trace health: {path} game {game_id}")
        pylon_created = 0
        with log.open(errors="replace") as stream:
            for line in stream:
                if line.startswith("LIFECYCLE,"):
                    fields = line.rstrip().split(",")
                    pylon_created += len(fields) > 5 and fields[2:4] == ["create", "self"] and \
                        fields[5] == "Protoss_Pylon"
        rows.append({"game_id": game_id, "seed": int(item["match"]["seed"]),
                     "map_hash": item["match"]["map_hash"], "map": game["map"],
                     "home": game["homeBot"], "frame": own["finalFrame"],
                     "capped": capped, "decisive_won": own["won"] if decisive else None,
                     "states": item["states"], "first_core": item["first_core"],
                     "first_defender": item["first_defender"],
                     "early_losses": item["early_losses"],
                     "pylons_created": pylon_created,
                     "delayed_pylons_created": delayed_pylons(log),
                     "log_sha256": item["log_sha256"]})
    components = manifest["components"]
    common = {key: value for key, value in components.items()
              if key != "server/bots/Protodd/AI/Protodd.dll"}
    return {"manifest_sha256": sha256(path / "manifest.json"),
            "dll_sha256": components["server/bots/Protodd/AI/Protodd.dll"],
            "common_inputs_sha256": hashlib.sha256(
                json.dumps(common, sort_keys=True).encode()).hexdigest(),
            "rows": rows}


def review(plan_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    for relative, digest in plan["sha256"].items():
        if sha256(root / relative) != digest:
            raise ValueError(f"frozen input changed: {relative}")
    results = {name: campaign(root / name, plan["games_per_arm"], plan["frame_limit"])
               for name in ("reference-a", "reference-b", "candidate")}
    a, b, c = (results[name] for name in ("reference-a", "reference-b", "candidate"))
    common_inputs = a["common_inputs_sha256"] == b["common_inputs_sha256"] == c["common_inputs_sha256"]
    dlls = a["dll_sha256"] == b["dll_sha256"] == plan["reference_dll_sha256"] and \
        c["dll_sha256"] == plan["candidate_dll_sha256"]
    paired = all(
        a["rows"][i][key] == b["rows"][i][key] == c["rows"][i][key]
        for i in range(plan["games_per_arm"])
        for key in ("seed", "map_hash", "map", "home"))
    seed_rule = all(a["rows"][i]["seed"] == plan["seed_base"] + i
                    for i in range(plan["games_per_arm"]))
    if not (common_inputs and dlls and paired and seed_rule):
        raise ValueError(f"source or match pairing differs: {common_inputs}, {dlls}, {paired}, {seed_rule}")
    comparison = []
    def metric_delta(left, right, name):
        if "8400" not in left["states"] or "8400" not in right["states"]:
            return None
        return right["states"]["8400"][name] - left["states"]["8400"][name]

    for i in range(plan["games_per_arm"]):
        x, y, z = (result["rows"][i] for result in (a, b, c))
        comparison.append({
            "game_id": i, "seed": x["seed"], "map": x["map"], "home": x["home"],
            "same_dll_army_delta_8400": metric_delta(x, y, "army"),
            "candidate_army_delta_8400": metric_delta(x, z, "army"),
            "same_dll_probe_delta_8400": metric_delta(x, y, "probes"),
            "candidate_probe_delta_8400": metric_delta(x, z, "probes"),
            "same_dll_early_loss_delta": y["early_losses"] - x["early_losses"],
            "candidate_early_loss_delta": z["early_losses"] - x["early_losses"],
            "same_dll_pylon_delta": y["pylons_created"] - x["pylons_created"],
            "candidate_pylon_delta": z["pylons_created"] - x["pylons_created"],
            "decisive_wins": [x["decisive_won"], y["decisive_won"], z["decisive_won"]],
            "frames": [x["frame"], y["frame"], z["frame"]],
        })
    candidate_delayed = sum(len(row["delayed_pylons_created"]) for row in c["rows"])
    return {"schema": "protodd-pvz-approach-generalization-review-v1",
            "plan_sha256": sha256(plan_path), "source_sha256": sha256(__file__),
            "trace_source_sha256": sha256(Path(__file__).with_name("pvz_core_transition_review.py")),
            "delayed_source_sha256": sha256(Path(__file__).with_name("pvz_approach_grace_screen.py")),
            "checks": {"common_inputs": common_inputs, "dlls": dlls,
                       "paired": paired, "seed_rule": seed_rule},
            "arms": results, "comparison": comparison,
            "candidate_delayed_pylons": candidate_delayed,
            "functional_transfer": candidate_delayed >= plan["evaluation_gate"]["minimum_candidate_delayed_pylons"],
            "strength_validated": False, "promotion_allowed": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"checks": result["checks"],
                      "comparison": result["comparison"]}))
