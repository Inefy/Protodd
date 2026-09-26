"""Review a source-pinned early PvZ worker-cap development comparison."""

import argparse
import hashlib
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256


def trace(path):
    match = None
    states = {}
    first_defender = None
    early_losses = 0
    enemy_activity = False
    errors = []
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if fields[0] == "MATCH":
                match = dict(part.split("=", 1) for part in fields[1:] if "=" in part)
            elif fields[0] == "STATE":
                values = dict(part.split("=", 1) for part in fields[14:] if "=" in part)
                enemy_activity |= int(values.get("enemyVisibleArmy", 0)) > 0
                frame = int(fields[1])
                if frame in (3000, 6000):
                    states[str(frame)] = {
                        "probes": int(values["probes"].split("/")[0]),
                        "army": int(values["army"]),
                        "minerals": int(fields[7]),
                    }
            elif fields[0] == "LIFECYCLE" and len(fields) > 5:
                if (fields[2] == "complete" and fields[3] == "self" and
                        fields[5] in ("Protoss_Zealot", "Protoss_Photon_Cannon") and
                        first_defender is None):
                    first_defender = int(fields[1])
            elif fields[0] == "LOSS" and len(fields) > 2:
                early_losses += fields[2] == "self" and int(fields[1]) < 6000
            elif fields[0] == "ERROR":
                errors.append(line.rstrip("\n"))
    return dict(match=match, states=states, first_defender=first_defender,
                early_losses=early_losses, enemy_activity=enemy_activity,
                errors=errors, log=str(path.resolve()), log_sha256=sha256(path))


def campaign(path, games):
    path = Path(path)
    verify(path)
    inspection = inspect(path)
    manifest = json.loads((path / "manifest.json").read_text())
    schedule = {row["gameID"]: row for row in map(
        json.loads, (path / "server/games.jsonl").read_text().splitlines())}
    rows = []
    for game in inspection["structurally_valid"]:
        gid = game["game_id"]
        log = path / f"server/replays/bot-write/game-{gid}/Protodd/received/Protodd.log"
        rows.append(dict(**game, **trace(log),
                         host=schedule[gid]["homeBot"] == "Protodd",
                         map=schedule[gid]["map"]))
    components = manifest["components"]
    dll = "server/bots/Protodd/AI/Protodd.dll"
    common = {key: value for key, value in components.items() if key != dll}
    common_hash = hashlib.sha256(json.dumps(common, sort_keys=True).encode()).hexdigest()
    return dict(path=str(path.resolve()), manifest_sha256=sha256(path / "manifest.json"),
                common_inputs_sha256=common_hash, dll_sha256=components[dll],
                healthy=(manifest["purpose"] == "development" and
                         manifest["games"] == games and inspection["scheduled"] == games and
                         len(rows) == games and not inspection["excluded"]),
                excluded=inspection["excluded"], rows=rows)


def review(plan_path, reference_path, candidate_path):
    plan_path = Path(plan_path)
    plan = json.loads(plan_path.read_text())
    n = plan["games_per_condition"]
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    healthy = reference["healthy"] and candidate["healthy"]
    identical_inputs = (reference["common_inputs_sha256"] == candidate["common_inputs_sha256"] and
                        reference["dll_sha256"] == plan["reference_dll_sha256"] and
                        candidate["dll_sha256"] == plan["candidate_dll_sha256"])
    paired = (healthy and all(
        ref[i]["match"] == cand[i]["match"] and
        ref[i]["host"] == cand[i]["host"] and ref[i]["map"] == cand[i]["map"] and
        int(ref[i]["match"]["seed"]) == plan["seed_base"] + i
        for i in range(n)))
    runtime = paired and all(
        row["enemy_activity"] and not row["errors"] and
        all(str(frame) in row["states"] for frame in (3000, 6000)) and
        row["first_defender"] is not None
        for row in (*ref.values(), *cand.values()))
    checks = dict(healthy=healthy, identical_inputs=identical_inputs,
                  paired=paired, runtime=runtime)
    result = dict(schema="protodd-pvz-worker-cap-review-v1",
                  plan_sha256=sha256(plan_path), review_source_sha256=sha256(__file__),
                  reference=reference, candidate=candidate, checks=checks,
                  screen_more_games=False, promotion_allowed=False)
    if not all(checks.values()):
        return result
    probe_gains_3000 = [cand[i]["states"]["3000"]["probes"] -
                        ref[i]["states"]["3000"]["probes"] for i in range(n)]
    probe_gains_6000 = [cand[i]["states"]["6000"]["probes"] -
                        ref[i]["states"]["6000"]["probes"] for i in range(n)]
    defender_delay = [cand[i]["first_defender"] - ref[i]["first_defender"]
                      for i in range(n)]
    extra_early_losses = [cand[i]["early_losses"] - ref[i]["early_losses"]
                          for i in range(n)]
    wins = [sum(rows[i]["won"] for i in range(n)) for rows in (ref, cand)]
    result["metrics"] = dict(probe_gains_3000=probe_gains_3000,
                             probe_gains_6000=probe_gains_6000,
                             defender_delay=defender_delay,
                             extra_early_losses=extra_early_losses,
                             wins_reference=wins[0], wins_candidate=wins[1])
    result["screen_more_games"] = (
        sum(gain >= 1 for gain in probe_gains_3000) >= 3 and
        sum(probe_gains_6000) / n >= 1 and
        sum(defender_delay) / n <= 240 and
        all(losses <= 0 for losses in extra_early_losses) and
        wins[1] >= wins[0])
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.reference, args.candidate)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result.get(key) for key in
                      ("checks", "metrics", "screen_more_games")}))
