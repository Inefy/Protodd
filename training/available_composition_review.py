"""Source-pinned functional screen with a repeated, identical-DLL control."""

import argparse
import itertools
import json
import statistics
from pathlib import Path

from .pvz_approach_generalization_review import campaign
from .schema import sha256


def production_events(path):
    accepted = []
    with path.open(errors="replace") as stream:
        for line in stream:
            fields = line.rstrip().split(",")
            if len(fields) > 7 and fields[0] == "MACRO" and fields[6] == "1" and \
                    fields[7] == "fill idle production with available tech composition":
                accepted.append({"frame": int(fields[1]), "kind": fields[3]})
    return accepted


def review(plan_path):
    root = plan_path.parent
    plan = json.loads(plan_path.read_text())
    for relative, digest in plan["sha256"].items():
        if sha256(root / relative) != digest:
            raise ValueError(f"Frozen input changed: {relative}")
    for path, digest in plan["repo_sha256"].items():
        if sha256(Path(path)) != digest:
            raise ValueError(f"Reviewer or launcher changed: {path}")
    arms = {name: campaign(root / name, plan["games_per_arm"], plan["frame_limit"])
            for name in ("reference-a", "reference-b", "candidate")}
    a, b, c = (arms[name] for name in arms)
    checks = {
        "common_inputs": a["common_inputs_sha256"] == b["common_inputs_sha256"] == c["common_inputs_sha256"],
        "dlls": a["dll_sha256"] == b["dll_sha256"] == plan["reference_dll_sha256"] and
                c["dll_sha256"] == plan["candidate_dll_sha256"],
        "paired": all(a["rows"][i][key] == b["rows"][i][key] == c["rows"][i][key]
                      for i in range(plan["games_per_arm"])
                      for key in ("seed", "map_hash", "map", "home")),
        "seed_rule": all(row["seed"] == plan["seed_base"] + row["game_id"] for row in a["rows"]),
    }
    if not all(checks.values()):
        raise ValueError(f"Input or pairing failed: {checks}")
    for name, arm in arms.items():
        for row in arm["rows"]:
            log = root / name / f"server/replays/bot-write/game-{row['game_id']}/Protodd/received/Protodd.log"
            row["accepted_composition_events"] = production_events(log)
    # A missing frame-8400 state due to an early loss is a failed functional
    # screen, never silently removed from the comparison.
    checks["all_reach_8400"] = all("8400" in row["states"] for arm in arms.values() for row in arm["rows"])
    result = {"schema": "protodd-available-composition-review-v1", "plan_sha256": sha256(plan_path),
              "arms": arms, "checks": checks, "functional_pass": False,
              "strength_validated": False, "promotion_allowed": False}
    if not checks["all_reach_8400"]:
        return result
    def mean_state(arm, frame, metric):
        return statistics.mean(row["states"][str(frame)][metric] for row in arm["rows"])
    def mean_core(arm):
        values = [row["first_core"] for row in arm["rows"] if row["first_core"] is not None]
        return statistics.mean(values) if values else None
    summaries = {name: {
        "army8400": mean_state(arm, 8400, "army"),
        "probes6000": mean_state(arm, 6000, "probes"),
        "probes8400": mean_state(arm, 8400, "probes"),
        "missing_cores": sum(row["first_core"] is None for row in arm["rows"]),
        "mean_core_frame": mean_core(arm),
        "median_defender_frame": statistics.median(row["first_defender"] for row in arm["rows"]),
        "early_losses": sum(row["early_losses"] for row in arm["rows"]),
        "decisive_wins": sum(row["decisive_won"] is True for row in arm["rows"]),
        "decisive_games": sum(row["decisive_won"] is not None for row in arm["rows"]),
        "exposed_games": sum(bool(row["accepted_composition_events"]) for row in arm["rows"]),
    } for name, arm in arms.items()}
    x, y, z = (summaries[name] for name in arms)
    army_effects = [r["states"]["8400"]["army"] -
                    (p["states"]["8400"]["army"] + q["states"]["8400"]["army"]) / 2
                    for p, q, r in zip(a["rows"], b["rows"], c["rows"])]
    # Exact resampling of the four independent seed groups; repeated reference
    # observations stay together. Descriptive uncertainty, not a strength gate.
    means = sorted(statistics.mean(values) for values in itertools.product(army_effects, repeat=len(army_effects)))
    gates = plan["gates"]
    checks.update({
        "exposure": z["exposed_games"] >= gates["min_exposed_games"],
        "army_gain": z["army8400"] >= max(x["army8400"], y["army8400"]) + gates["min_army_gain"],
        "workers_preserved": all(z[key] >= min(x[key], y[key]) - gates["max_mean_probe_drop"]
                                 for key in ("probes6000", "probes8400")),
        "defender_preserved": z["median_defender_frame"] <=
            max(x["median_defender_frame"], y["median_defender_frame"]) + gates["max_median_defender_delay"],
        "cores_preserved": z["missing_cores"] <= max(x["missing_cores"], y["missing_cores"]),
        "core_timing": all(summary["mean_core_frame"] is not None for summary in (x, y, z)) and
            z["mean_core_frame"] <= max(x["mean_core_frame"], y["mean_core_frame"]) + gates["max_mean_core_delay"],
        "early_losses": z["early_losses"] <= max(x["early_losses"], y["early_losses"]) + gates["max_total_extra_early_losses"],
        "survival": all(r["frame"] >= min(p["frame"], q["frame"]) - gates["max_survival_drop"]
                        for p, q, r in zip(a["rows"], b["rows"], c["rows"])),
        "outcomes": z["decisive_wins"] >= max(x["decisive_wins"], y["decisive_wins"]),
        "both_maps": all(statistics.mean(army_effects[i] for i, row in enumerate(a["rows"]) if row["map"] == map_name) >= 0
                         for map_name in {row["map"] for row in a["rows"]}),
    })
    result.update(summaries=summaries, army_effects=army_effects,
                  army_effect_bootstrap_95=[means[int(.025 * (len(means)-1))], means[int(.975 * (len(means)-1))]],
                  functional_pass=all(checks.values()))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result.get(key) for key in ("checks", "summaries", "functional_pass")}))
