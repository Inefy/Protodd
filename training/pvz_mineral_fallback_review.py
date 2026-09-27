"""Review an opt-in PvZ mineral-surplus spending screen.

Only structurally valid, same-seed games supply descriptive timing metrics.
An excluded game prevents a full paired or promotion gate from passing.
"""

import argparse
import json
from pathlib import Path

from tools.log_analyzer import parse_key_values

from .pvz_replay_opening_review import campaign


def spending(path):
    result = dict(high_bank_idle_samples=0, idle_gateway_samples=0,
                  zealots_completed=0, late_army=None, late_minerals=None,
                  old_window_ready_samples=0, early_window_ready_samples=0,
                  fallback_mentions=0)
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            fields = line.rstrip("\n").split(",")
            if "spend mineral surplus in idle PvZ Gateways" in line:
                result["fallback_mentions"] += 1
            if fields[0] not in ("HEALTH", "LIFECYCLE", "STATE"):
                continue
            frame = int(fields[1])
            if frame < 12000 or frame > 18000:
                continue
            if fields[0] == "HEALTH":
                values = parse_key_values(fields[2:])
                idle = int(values.get("idleGateways", "0"))
                result["idle_gateway_samples"] += idle
                result["high_bank_idle_samples"] += (
                    int(values.get("minerals", "0")) >= 800 and idle >= 2)
            elif fields[0] == "LIFECYCLE" and len(fields) > 5:
                result["zealots_completed"] += (
                    fields[2] == "complete" and fields[3] == "self" and
                    fields[5] == "Protoss_Zealot")
            elif fields[0] == "STATE":
                values = parse_key_values(fields[14:])
                bank = int(fields[7])
                free_minerals, free_gas = map(int, str(values["ledger"]).split("/")[:2])
                supply_room = int(fields[10]) - int(fields[9])
                cannons = int(str(values["cannons"]).split("/")[1])
                probes = int(str(values["probes"]).split("/")[0])
                core = int(str(values["core"]).split("/")[1])
                common = (bank >= 800 and free_minerals >= 600 and
                          int(values["gateways"]) >= 4 and supply_room >= 4)
                if common and frame >= 17280 and free_gas < 125 and \
                        int(values["nexuses"]) >= 1:
                    result["old_window_ready_samples"] += 1
                if common and frame >= 12000 and \
                        int(values["nexuses"]) >= 2 and core >= 1 and \
                        cannons >= 2 and probes >= 26:
                    result["early_window_ready_samples"] += 1
                if frame == 18000:
                    result["late_army"] = int(values["army"])
                    result["late_minerals"] = bank
    return result


def review(reference_path, candidate_path):
    reference, candidate = campaign(reference_path), campaign(candidate_path)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    inputs_match = reference["inputs"] == candidate["inputs"]
    matched = [i for i in sorted(ref.keys() & cand.keys()) if
               inputs_match and ref[i]["map"] == cand[i]["map"] and
               ref[i]["host"] == cand[i]["host"] and
               ref[i]["match"] == cand[i]["match"]]
    paired = (reference["healthy"] and candidate["healthy"] and
              len(matched) == len(ref) == len(cand))
    observed = [i for i in matched if ref[i]["enemy_activity"] and
                cand[i]["enemy_activity"] and not ref[i]["errors"] and
                not cand[i]["errors"]]
    for rows in (ref, cand):
        for row in rows.values():
            row["spending"] = spending(row["log"])
    exposed = [i for i in observed if ref[i]["frame"] >= 18000 and
               cand[i]["frame"] >= 18000]
    metrics = dict(game_ids=observed, exposed_games=exposed,
                   reference={str(i): ref[i]["spending"] for i in observed},
                   candidate={str(i): cand[i]["spending"] for i in observed},
                   extra_early_losses={str(i): cand[i]["early_losses"] -
                                       ref[i]["early_losses"] for i in observed},
                   wins_reference=sum(ref[i]["won"] for i in observed),
                   wins_candidate=sum(cand[i]["won"] for i in observed))
    functional = (paired and len(exposed) >= 2 and
                  sum(cand[i]["spending"]["high_bank_idle_samples"] * 4 <=
                      ref[i]["spending"]["high_bank_idle_samples"] * 3
                      for i in exposed) >= 2 and
                  all(loss <= 0 for loss in metrics["extra_early_losses"].values()))
    return dict(schema="protodd-pvz-mineral-fallback-review-v1",
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
