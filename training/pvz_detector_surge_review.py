"""Review emergency Observer escort allocation in a frozen PvZ comparison."""

import argparse
import json
from pathlib import Path

from .pvz_powered_cannon_review import defense
from .pvz_replay_opening_review import campaign


def detection(path):
    first_lurker = None
    observer_ids = set()
    reaver_ids = set()
    scout_orders = []
    escort_orders = []
    attack_orders = []
    blocked_main_unit_samples = 0
    main_unit_samples = 0
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("LIFECYCLE,"):
                fields = line.rstrip("\n").split(",")
                if len(fields) < 6:
                    continue
                frame = int(fields[1])
                if (fields[2] in ("discover", "show") and fields[3] == "enemy" and
                        fields[5] == "Zerg_Lurker" and first_lurker is None):
                    first_lurker = frame
                if fields[2] == "complete" and fields[3] == "self":
                    if fields[5] == "Protoss_Observer":
                        observer_ids.add(fields[4])
                    elif fields[5] == "Protoss_Reaver":
                        reaver_ids.add(fields[4])
            elif line.startswith("SQUAD,"):
                fields = line.rstrip("\n").split(",")
                if fields[2] != "MainArmy" or not 14000 <= int(fields[1]) <= 21000:
                    continue
                values = dict(field.split("=", 1) for field in fields[3:] if "=" in field)
                units = int(values.get("units", "0"))
                if int(values.get("enemies", "0")) == 0:
                    continue
                main_unit_samples += units
                if values.get("detectionBlocked") == "1":
                    blocked_main_unit_samples += units
            elif line.startswith("ACTION,"):
                fields = line.rstrip("\n").split(",")
                if len(fields) < 7 or fields[5:7] != ["issued", "accepted"]:
                    continue
                frame = int(fields[1])
                if not 14000 <= frame <= 21000:
                    continue
                if fields[3] == "scout-travel" and fields[4] == "Move":
                    scout_orders.append((frame, fields[2]))
                elif fields[3] == "detector-escort" and fields[4] == "Move":
                    escort_orders.append((frame, fields[2]))
                elif fields[4] == "Attack_Unit":
                    attack_orders.append((frame, fields[2]))
    after_lurker = lambda row: first_lurker is not None and row[0] >= first_lurker
    scout_orders = [row for row in scout_orders if after_lurker(row) and row[1] in observer_ids]
    escort_orders = [row for row in escort_orders if after_lurker(row) and row[1] in observer_ids]
    reaver_attacks = [row for row in attack_orders if row[1] in reaver_ids]
    return dict(first_lurker=first_lurker,
                scout_observers=sorted({actor for _, actor in scout_orders}),
                scout_orders=len(scout_orders),
                escort_observers=sorted({actor for _, actor in escort_orders}),
                escort_orders=len(escort_orders),
                blocked_main_unit_samples=blocked_main_unit_samples,
                main_unit_samples=main_unit_samples,
                blocked_main_fraction=(blocked_main_unit_samples / main_unit_samples
                                       if main_unit_samples else None),
                reaver_attacks=len(reaver_attacks))


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
            row["detection"] = detection(row["log"])
    exposed = [i for i in observed if refs[i]["detection"]["first_lurker"] is not None and
               cands[i]["detection"]["first_lurker"] is not None]
    metrics = dict(game_ids=observed, exposed_games=exposed,
                   reference={str(i): dict(defense=refs[i]["defense"],
                                           detection=refs[i]["detection"]) for i in observed},
                   candidate={str(i): dict(defense=cands[i]["defense"],
                                           detection=cands[i]["detection"]) for i in observed},
                   extra_early_losses={str(i): cands[i]["early_losses"] -
                                       refs[i]["early_losses"] for i in observed},
                   wins_reference=sum(refs[i]["won"] for i in observed),
                   wins_candidate=sum(cands[i]["won"] for i in observed))
    improved_detection = [i for i in exposed if
                          refs[i]["detection"]["blocked_main_fraction"] is not None and
                          cands[i]["detection"]["blocked_main_fraction"] is not None and
                          cands[i]["detection"]["blocked_main_fraction"] <
                          refs[i]["detection"]["blocked_main_fraction"]]
    cannon_starts_safe = all(
        len(refs[i]["defense"]["started"]) >= 2 and
        len(cands[i]["defense"]["started"]) >= 2 and
        all(cands[i]["defense"]["started"][n] <=
            refs[i]["defense"]["started"][n] + 300 for n in range(2))
        for i in observed)
    functional = (paired and bool(improved_detection) and cannon_starts_safe and
                  all(loss <= 0 for loss in metrics["extra_early_losses"].values()))
    return dict(schema="protodd-pvz-detector-surge-review-v1",
                reference=reference, candidate=candidate,
                checks=dict(inputs_match=inputs_match, matched_games=matched,
                            paired=paired), metrics=metrics,
                improved_detection_games=improved_detection,
                cannon_starts_safe=cannon_starts_safe,
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
                      ("checks", "improved_detection_games", "cannon_starts_safe",
                       "functional_pass", "screen_more_games")}))
