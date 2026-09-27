"""Review a proactive first-Reaver screen behind the powered PvZ natural."""

import argparse
import json
from pathlib import Path

from .pvz_powered_cannon_review import defense
from .pvz_replay_opening_review import campaign


def splash(path):
    first = dict(robotics=None, support_bay=None, reaver=None,
                 hydralisk_seen=None, first_reaver_ammo=None,
                 first_reaver_attack=None, accepted_reaver_attacks=0)
    first_reaver_id = None
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if first_reaver_id is not None and line.startswith("ENTITY,") and \
                    f",self,{first_reaver_id},Reaver," in line and \
                    first["first_reaver_ammo"] is None:
                fields = line.rstrip("\n").split(",")
                ammo = next((int(value.removeprefix("ammo=")) for value in fields
                             if value.startswith("ammo=")), 0)
                if ammo > 0:
                    first["first_reaver_ammo"] = int(fields[1])
            if first_reaver_id is not None and line.startswith("ACTION,") and \
                    f",{first_reaver_id}," in line and \
                    ",Attack_Unit,issued,accepted," in line:
                first["accepted_reaver_attacks"] += 1
                if first["first_reaver_attack"] is None:
                    first["first_reaver_attack"] = int(line.split(",", 2)[1])
            if not line.startswith("LIFECYCLE,"):
                continue
            fields = line.rstrip("\n").split(",")
            if len(fields) < 6:
                continue
            frame = int(fields[1])
            if fields[2] == "complete" and fields[3] == "self":
                for key, kind in (("robotics", "Protoss_Robotics_Facility"),
                                  ("support_bay", "Protoss_Robotics_Support_Bay"),
                                  ("reaver", "Protoss_Reaver")):
                    if fields[5] == kind and first[key] is None:
                        first[key] = frame
                        if key == "reaver":
                            first_reaver_id = fields[4]
            if fields[2] in ("discover", "show") and fields[3] == "enemy" and \
                    fields[5] == "Zerg_Hydralisk" and first["hydralisk_seen"] is None:
                first["hydralisk_seen"] = frame
    return first


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
            row["splash"] = splash(row["log"])
    metrics = dict(game_ids=observed,
                   reference={str(i): dict(defense=refs[i]["defense"],
                                           splash=refs[i]["splash"]) for i in observed},
                   candidate={str(i): dict(defense=cands[i]["defense"],
                                           splash=cands[i]["splash"]) for i in observed},
                   extra_early_losses={str(i): cands[i]["early_losses"] -
                                       refs[i]["early_losses"] for i in observed},
                   wins_reference=sum(refs[i]["won"] for i in observed),
                   wins_candidate=sum(cands[i]["won"] for i in observed))
    timely = [i for i in observed if cands[i]["splash"]["reaver"] is not None and
              cands[i]["splash"]["reaver"] <= 12000]
    first_cannons_safe = all(
        len(refs[i]["defense"]["started"]) >= 3 and
        len(cands[i]["defense"]["started"]) >= 3 and
        all(cands[i]["defense"]["started"][n] <=
            refs[i]["defense"]["started"][n] + 300 for n in range(3))
        for i in observed)
    natural_cannons_safe = all(
        refs[i]["defense"]["first_natural_cannon_completed"] is None or
        (cands[i]["defense"]["first_natural_cannon_completed"] is not None and
         cands[i]["defense"]["first_natural_cannon_completed"] <=
         refs[i]["defense"]["first_natural_cannon_completed"] + 300)
        for i in observed)
    functional = (paired and len(timely) >= 2 and first_cannons_safe and
                  natural_cannons_safe and
                  all(loss <= 0 for loss in metrics["extra_early_losses"].values()))
    return dict(schema="protodd-pvz-proactive-reaver-review-v1",
                reference=reference, candidate=candidate,
                checks=dict(inputs_match=inputs_match, matched_games=matched,
                            paired=paired), metrics=metrics,
                timely_reaver_games=timely,
                first_cannons_safe=first_cannons_safe,
                natural_cannons_safe=natural_cannons_safe,
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
                      ("checks", "timely_reaver_games", "first_cannons_safe",
                       "natural_cannons_safe",
                       "functional_pass", "screen_more_games")}))
