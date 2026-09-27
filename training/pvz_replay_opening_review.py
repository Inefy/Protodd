"""Compare replay-derived PvZ opening timings in frozen local campaigns.

Only paired, healthy development games count. Milestones describe mechanism;
they are not evidence of improved strength without game wins.
"""

import argparse
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256
from tools.log_analyzer import parse_key_values


MILESTONES = (4800, 6000, 7200, 8400, 9600)
PORT_SETTINGS = ("server/server_settings.json", "client1/client_settings.json",
                 "client2/client_settings.json")


def frozen_inputs(path, manifest):
    components = {name: digest for name, digest in manifest["components"].items()
                  if name != "server/bots/Protodd/AI/Protodd.dll" and
                  name not in PORT_SETTINGS}
    settings = {}
    for name in PORT_SETTINGS:
        value = json.loads((path / name).read_text())
        value.pop("serverPort" if name.startswith("server/") else "ServerAddress")
        settings[name] = value
    return dict(components=components, normalized_settings=settings)


def trace(path):
    states = {}
    match = None
    natural_started = None
    first_cannon = None
    early_losses = 0
    enemy_activity = False
    errors = []
    with path.open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if fields[0] == "MATCH":
                match = parse_key_values(fields[1:])
            elif fields[0] == "STATE" and len(fields) >= 15:
                frame = int(fields[1])
                values = parse_key_values(fields[14:])
                enemy_activity |= int(values.get("enemyVisibleArmy", "0")) > 0
                if frame in MILESTONES:
                    states[str(frame)] = dict(
                        probes=int(str(values["probes"]).split("/")[0]),
                        nexuses=int(values["nexuses"]),
                        gateways=int(values["gateways"]),
                        cannons=int(str(values["cannons"]).split("/")[0]),
                        zealots=int(str(values["zealots"]).split("/")[0]),
                        army=int(values["army"]),
                        minerals=int(fields[7]),
                        strategy=fields[2],
                    )
            elif fields[0] == "LIFECYCLE" and len(fields) > 5 and fields[3] == "self":
                if fields[2] == "create" and fields[5] == "Protoss_Nexus" and int(fields[1]) > 0:
                    natural_started = natural_started or int(fields[1])
                if fields[2] == "create" and fields[5] == "Protoss_Photon_Cannon":
                    first_cannon = first_cannon or int(fields[1])
            elif fields[0] == "LOSS" and len(fields) > 2:
                early_losses += fields[2] == "self" and int(fields[1]) < 6000
            elif fields[0] == "ERROR":
                errors.append(line.rstrip("\n"))
    return dict(match=match, states=states, natural_started=natural_started,
                first_cannon=first_cannon, early_losses=early_losses,
                enemy_activity=enemy_activity, errors=errors,
                log=str(path.resolve()), log_sha256=sha256(path))


def campaign(path):
    verify(path)
    inspection = inspect(path)
    manifest = json.loads((path / "manifest.json").read_text())
    schedule = {row["gameID"]: row for row in map(
        json.loads, (path / "server/games.jsonl").read_text().splitlines())}
    rows = []
    for game in inspection["structurally_valid"]:
        gid = game["game_id"]
        log = path / f"server/replays/bot-write/game-{gid}/Protodd/received/Protodd.log"
        rows.append(dict(**game, **trace(log), host=schedule[gid]["homeBot"] == "Protodd",
                         map=schedule[gid]["map"]))
    return dict(path=str(path.resolve()),
                manifest_sha256=sha256(path / "manifest.json"),
                dll_sha256=manifest["components"]["server/bots/Protodd/AI/Protodd.dll"],
                inputs=frozen_inputs(path, manifest),
                healthy=(manifest["purpose"] == "development" and
                         manifest["games"] == len(rows) == inspection["scheduled"] and
                         not inspection["excluded"]),
                rows=rows, excluded=inspection["excluded"])


def review(reference, candidate):
    ref, cand = campaign(reference), campaign(candidate)
    refs = {row["game_id"]: row for row in ref["rows"]}
    cands = {row["game_id"]: row for row in cand["rows"]}
    inputs_match = ref["inputs"] == cand["inputs"]
    matched = [i for i in sorted(refs.keys() & cands.keys()) if
               inputs_match and refs[i]["map"] == cands[i]["map"] and
               refs[i]["host"] == cands[i]["host"] and
               refs[i]["match"] == cands[i]["match"]]
    paired = (ref["healthy"] and cand["healthy"] and
              len(matched) == len(refs) == len(cands))
    runtime = paired and all(row["enemy_activity"] and not row["errors"] and
                             "4800" in row["states"] and "7200" in row["states"]
                             for row in (*refs.values(), *cands.values()))
    observed = [i for i in matched if all(
        row["enemy_activity"] and not row["errors"] and
        "4800" in row["states"] and "7200" in row["states"]
        for row in (refs[i], cands[i]))]
    metrics = {}
    if observed:
        metrics = dict(
            game_ids=observed,
            probe_gain_4800=[cands[i]["states"]["4800"]["probes"] -
                             refs[i]["states"]["4800"]["probes"] for i in observed],
            probe_gain_7200=[cands[i]["states"]["7200"]["probes"] -
                             refs[i]["states"]["7200"]["probes"] for i in observed],
            natural_started_reference=[refs[i]["natural_started"] for i in observed],
            natural_started_candidate=[cands[i]["natural_started"] for i in observed],
            extra_early_losses=[cands[i]["early_losses"] - refs[i]["early_losses"]
                                for i in observed],
            wins_reference=sum(refs[i]["won"] for i in observed),
            wins_candidate=sum(cands[i]["won"] for i in observed))
    mechanism = runtime and sum(gain >= 3 for gain in metrics["probe_gain_4800"]) >= 3 and \
        sum(frame is not None and frame <= 4800 for frame in
            metrics["natural_started_candidate"]) >= 3 and \
        all(loss <= 0 for loss in metrics["extra_early_losses"])
    return dict(schema="protodd-pvz-replay-opening-review-v1", reference=ref,
                candidate=cand, checks=dict(inputs_match=inputs_match,
                                            matched_games=matched,
                                            paired=paired, runtime=runtime),
                metrics=metrics, mechanism_pass=mechanism,
                screen_more_games=mechanism and
                metrics["wins_candidate"] > metrics["wins_reference"],
                promotion_allowed=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.reference, args.candidate)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: result[key] for key in
                      ("checks", "metrics", "mechanism_pass", "screen_more_games")}))
