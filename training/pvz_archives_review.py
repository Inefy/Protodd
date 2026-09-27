"""Review an Archives-before-drops PvZ development comparison.

Timings come from archived legal bot logs. A model or tactic is not promoted
from tech timing alone; paired outcomes and opponent activity still matter.
"""

import argparse
import json
from pathlib import Path

from .pvz_replay_opening_review import campaign


LIFECYCLE = {
    ("create", "Protoss_Templar_Archives"): "archive_started",
    ("complete", "Protoss_Templar_Archives"): "archive_complete",
    ("complete", "Protoss_High_Templar"): "first_templar",
    ("create", "Protoss_Robotics_Support_Bay"): "support_bay_started",
    ("complete", "Protoss_Reaver"): "first_reaver",
}


def timings(path):
    result = {name: None for name in LIFECYCLE.values()}
    result["first_storm"] = None
    with Path(path).open(encoding="utf-8", errors="replace") as stream:
        for line in stream:
            if line.startswith("LIFECYCLE,"):
                fields = line.split(",")
                if len(fields) > 5 and fields[3] == "self":
                    name = LIFECYCLE.get((fields[2], fields[5]))
                    if name and result[name] is None:
                        result[name] = int(fields[1])
            elif line.startswith("STATE,") and result["first_storm"] is None:
                fields = line.split(",")
                for field in fields[14:]:
                    if field.startswith("storm=") and field.split("=", 1)[1] != "0":
                        result["first_storm"] = int(fields[1])
                        break
    return result


def review(reference_path, candidate_path):
    reference, candidate = campaign(reference_path), campaign(candidate_path)
    ref = {row["game_id"]: row for row in reference["rows"]}
    cand = {row["game_id"]: row for row in candidate["rows"]}
    paired = reference["healthy"] and candidate["healthy"] and \
        reference["inputs"] == candidate["inputs"] and ref.keys() == cand.keys() and \
        all(ref[i]["map"] == cand[i]["map"] and
            ref[i]["host"] == cand[i]["host"] and
            ref[i]["match"] == cand[i]["match"] for i in ref)
    runtime = paired and all(row["enemy_activity"] and not row["errors"]
                             for row in (*ref.values(), *cand.values()))
    for rows in (ref, cand):
        for row in rows.values():
            row.update(timings(row["log"]))
    metrics = {}
    if runtime:
        metrics = dict(
            archive_complete_reference=[ref[i]["archive_complete"] for i in ref],
            archive_complete_candidate=[cand[i]["archive_complete"] for i in ref],
            first_templar_reference=[ref[i]["first_templar"] for i in ref],
            first_templar_candidate=[cand[i]["first_templar"] for i in ref],
            first_storm_reference=[ref[i]["first_storm"] for i in ref],
            first_storm_candidate=[cand[i]["first_storm"] for i in ref],
            support_bay_started_reference=[ref[i]["support_bay_started"] for i in ref],
            support_bay_started_candidate=[cand[i]["support_bay_started"] for i in ref],
            first_reaver_reference=[ref[i]["first_reaver"] for i in ref],
            first_reaver_candidate=[cand[i]["first_reaver"] for i in ref],
            extra_early_losses=[cand[i]["early_losses"] - ref[i]["early_losses"]
                                for i in ref],
            wins_reference=sum(ref[i]["won"] for i in ref),
            wins_candidate=sum(cand[i]["won"] for i in ref))
    exposed = [i for i in ref if ref[i]["frame"] >= 13200 and cand[i]["frame"] >= 13200]
    earlier_archives = runtime and sum(
        cand[i]["archive_complete"] is not None and
        (ref[i]["archive_complete"] is None or
         cand[i]["archive_complete"] + 720 <= ref[i]["archive_complete"])
        for i in exposed) >= 2
    first_templar = runtime and sum(
        cand[i]["first_templar"] is not None and cand[i]["first_templar"] <= 15600
        for i in exposed) >= 2
    functional = earlier_archives and first_templar and \
        all(loss <= 0 for loss in metrics["extra_early_losses"])
    return dict(schema="protodd-pvz-archives-review-v1", reference=reference,
                candidate=candidate, checks=dict(paired=paired, runtime=runtime),
                eligible_games=exposed, metrics=metrics, functional_pass=functional,
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
                      ("checks", "eligible_games", "metrics", "functional_pass",
                       "screen_more_games")}))
