"""Review the isolated PvZ army floor against the powered-Cannon reference."""

import argparse
import json
from pathlib import Path

from tools.log_analyzer import parse_key_values

from .pvz_powered_cannon_review import defense
from .pvz_replay_opening_review import campaign


CHECKPOINTS = (12000, 14400, 16800, 19200)
REASON = "spend two-base mineral surplus on ground defenders"


def army_floor(path):
    checkpoints = {}
    active_frames = []
    accepted_training = 0
    nexus_starts = []
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("STATE,"):
                fields = line.rstrip("\n").split(",")
                frame = int(fields[1])
                if "[two-base ground army floor]" in fields[2]:
                    active_frames.append(frame)
                if frame in CHECKPOINTS:
                    values = parse_key_values(fields[14:])
                    checkpoints[str(frame)] = dict(
                        army=int(values["army"]),
                        probes=int(str(values["probes"]).split("/")[0]),
                        nexuses=int(values["nexuses"]),
                        minerals=int(fields[7]),
                    )
            elif line.startswith("ACTION,") and f",{REASON},Train,issued,accepted," in line:
                accepted_training += 1
            elif line.startswith("LIFECYCLE,"):
                fields = line.rstrip("\n").split(",")
                if (len(fields) > 5 and fields[2] == "create" and
                        fields[3] == "self" and fields[5] == "Protoss_Nexus" and
                        int(fields[1]) > 0):
                    nexus_starts.append(int(fields[1]))
    return dict(first_active=active_frames[0] if active_frames else None,
                active_samples=len(active_frames),
                accepted_training=accepted_training,
                nexus_starts=nexus_starts,
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
            row["army_floor"] = army_floor(row["log"])
    metrics = dict(game_ids=observed,
                   reference={str(i): dict(defense=refs[i]["defense"],
                                           army_floor=refs[i]["army_floor"]) for i in observed},
                   candidate={str(i): dict(defense=cands[i]["defense"],
                                           army_floor=cands[i]["army_floor"]) for i in observed},
                   extra_early_losses={str(i): cands[i]["early_losses"] -
                                       refs[i]["early_losses"] for i in observed},
                   wins_reference=sum(refs[i]["won"] for i in observed),
                   wins_candidate=sum(cands[i]["won"] for i in observed))
    active = [i for i in observed if cands[i]["army_floor"]["active_samples"] > 0]
    funded = [i for i in active if cands[i]["army_floor"]["accepted_training"] > 0]
    cannon_starts_safe = all(
        len(refs[i]["defense"]["started"]) >= 2 and
        len(cands[i]["defense"]["started"]) >= 2 and
        all(cands[i]["defense"]["started"][n] <=
            refs[i]["defense"]["started"][n] + 300 for n in range(2))
        for i in observed)
    natural_safe = all(
        refs[i]["defense"]["first_natural_cannon_completed"] is None or
        (cands[i]["defense"]["first_natural_cannon_completed"] is not None and
         cands[i]["defense"]["first_natural_cannon_completed"] <=
         refs[i]["defense"]["first_natural_cannon_completed"] + 300)
        for i in observed)
    functional = (paired and bool(active) and bool(funded) and cannon_starts_safe and
                  natural_safe and
                  all(loss <= 0 for loss in metrics["extra_early_losses"].values()))
    return dict(schema="protodd-pvz-army-floor-review-v1",
                reference=reference, candidate=candidate,
                checks=dict(inputs_match=inputs_match, matched_games=matched,
                            paired=paired), metrics=metrics,
                active_games=active, funded_games=funded,
                cannon_starts_safe=cannon_starts_safe, natural_safe=natural_safe,
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
                      ("checks", "active_games", "funded_games",
                       "cannon_starts_safe", "natural_safe",
                       "functional_pass", "screen_more_games")}))
