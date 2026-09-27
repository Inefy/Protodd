"""Review the isolated staged-Cannon PvZ screen against frozen opening games."""

import argparse
import json
from pathlib import Path

from tools.log_analyzer import parse_key_values

from .pvz_replay_opening_review import campaign


CHECKPOINTS = (7200, 8400, 9600, 10800)


def defense(path):
    started = {}
    completed = {}
    positions = {}
    nexuses = {}
    checkpoints = {}
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if fields[0] == "LIFECYCLE" and len(fields) > 7 and \
                    fields[3] == "self":
                point = (int(fields[6].removeprefix("x=")),
                         int(fields[7].removeprefix("y=")))
                if fields[5] == "Protoss_Nexus" and fields[2] == "create":
                    nexuses.setdefault(fields[4], (int(fields[1]), point))
                elif fields[5] == "Protoss_Photon_Cannon":
                    if fields[2] == "create":
                        started.setdefault(fields[4], int(fields[1]))
                        positions.setdefault(fields[4], point)
                    elif fields[2] == "complete":
                        completed.setdefault(fields[4], int(fields[1]))
            elif fields[0] == "STATE" and len(fields) >= 15 and \
                    int(fields[1]) in CHECKPOINTS:
                values = parse_key_values(fields[14:])
                checkpoints[fields[1]] = dict(
                    cannons=int(str(values["cannons"]).split("/")[1]),
                    probes=int(str(values["probes"]).split("/")[0]),
                    nexuses=int(values["nexuses"]),
                    army=int(values["army"]),
                    enemy_visible=int(values["enemyVisibleArmy"]))
    natural = next((point for frame, point in sorted(nexuses.values())
                    if frame > 0), None)
    natural_cannons_9600 = sum(
        frame <= 9600 and natural is not None and
        (positions[identifier][0] - natural[0]) ** 2 +
        (positions[identifier][1] - natural[1]) ** 2 <= 416 ** 2
        for identifier, frame in completed.items() if identifier in positions)
    return dict(started=sorted(started.values()),
                completed=sorted(completed.values()),
                cannon_positions=[dict(id=identifier, started=frame,
                                       completed=completed.get(identifier),
                                       position=positions[identifier])
                                  for identifier, frame in sorted(
                                      started.items(), key=lambda row: row[1])],
                natural=natural, natural_cannons_9600=natural_cannons_9600,
                checkpoints=checkpoints)


def review(reference_path, candidate_path):
    reference, candidate = campaign(reference_path), campaign(candidate_path)
    refs = {row["game_id"]: row for row in reference["rows"]}
    cands = {row["game_id"]: row for row in candidate["rows"]}
    inputs_match = reference["inputs"] == candidate["inputs"]
    matched = [i for i in sorted(refs.keys() & cands.keys()) if
               inputs_match and refs[i]["map"] == cands[i]["map"] and
               refs[i]["host"] == cands[i]["host"] and
               refs[i]["match"] == cands[i]["match"]]
    paired = (reference["healthy"] and candidate["healthy"] and
              len(matched) == len(refs) == len(cands))
    observed = [i for i in matched if refs[i]["enemy_activity"] and
                cands[i]["enemy_activity"] and not refs[i]["errors"] and
                not cands[i]["errors"]]
    for rows in (refs, cands):
        for row in rows.values():
            row["defense"] = defense(row["log"])
    exposed = [i for i in observed if all(
        "9600" in row["defense"]["checkpoints"] for row in (refs[i], cands[i]))]
    metrics = dict(game_ids=observed, exposed_games=exposed,
                   reference={str(i): refs[i]["defense"] for i in observed},
                   candidate={str(i): cands[i]["defense"] for i in observed},
                   extra_early_losses={str(i): cands[i]["early_losses"] -
                                       refs[i]["early_losses"] for i in observed},
                   wins_reference=sum(refs[i]["won"] for i in observed),
                   wins_candidate=sum(cands[i]["won"] for i in observed))
    # A small selected replay slice suggests a third Cannon before 9600;
    # only complete, matched arena games can justify further screening.
    functional = (paired and len(exposed) >= 2 and
                  sum(cands[i]["defense"]["checkpoints"]["9600"]["cannons"] >= 3
                      for i in exposed) >= 2 and
                  all(loss <= 0 for loss in metrics["extra_early_losses"].values()))
    return dict(schema="protodd-pvz-powered-cannon-review-v1",
                reference=reference, candidate=candidate,
                checks=dict(inputs_match=inputs_match, matched_games=matched,
                            paired=paired), metrics=metrics,
                functional_pass=functional,
                screen_more_games=functional and
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
                      ("checks", "metrics", "functional_pass",
                       "screen_more_games")}))
