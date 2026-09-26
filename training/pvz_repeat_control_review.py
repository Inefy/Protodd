"""Audit run-to-run variance with the same PvZ DLL and paired StarCraft seeds."""

import argparse
import json
from pathlib import Path

from .pvz_core_transition_review import campaign
from .schema import sha256


def early_states(path):
    states = {}
    with Path(path).open(errors="replace") as stream:
        for line in stream:
            if not line.startswith("STATE,"):
                continue
            fields = line.rstrip("\n").split(",")
            frame = int(fields[1])
            if frame >= 7200:
                continue
            values = dict(part.split("=", 1) for part in fields[14:] if "=" in part)
            states[frame] = (
                fields[7], values.get("probes"), values.get("army"),
                values.get("zealots"), values.get("core"),
                values.get("nexuses"), values.get("gateways"),
            )
    return states


def review(plan_path, original_path, repeat_path):
    plan_path = Path(plan_path)
    plan = json.loads(plan_path.read_text())
    original = campaign(original_path, plan["games"])
    repeat = campaign(repeat_path, plan["games"])
    first = {row["game_id"]: row for row in original["rows"]}
    second = {row["game_id"]: row for row in repeat["rows"]}
    same_inputs = (
        original["common_inputs_sha256"] == repeat["common_inputs_sha256"] and
        original["dll_sha256"] == repeat["dll_sha256"] == plan["dll_sha256"]
    )
    paired = original["healthy"] and repeat["healthy"] and all(
        first[i]["match"] == second[i]["match"] and
        first[i]["host"] == second[i]["host"] and
        first[i]["map"] == second[i]["map"] and
        int(first[i]["match"]["seed"]) == plan["seed_base"] + i
        for i in range(plan["games"])
    )
    rows = []
    if same_inputs and paired:
        for i in range(plan["games"]):
            a, b = first[i], second[i]
            left, right = early_states(a["log"]), early_states(b["log"])
            frames = sorted(set(left) | set(right))
            mismatch = [frame for frame in frames if left.get(frame) != right.get(frame)]
            rows.append(dict(
                game_id=i, seed=a["match"]["seed"], map=a["map"], host=a["host"],
                first_state_divergence=mismatch[0] if mismatch else None,
                differing_early_states=len(mismatch), early_state_frames=len(frames),
                army_delta_6000=b["states"]["6000"]["army"] - a["states"]["6000"]["army"],
                probe_delta_6000=b["states"]["6000"]["probes"] - a["states"]["6000"]["probes"],
                defender_delta=(None if a["first_defender"] is None or
                                b["first_defender"] is None else
                                b["first_defender"] - a["first_defender"]),
                early_loss_delta=b["early_losses"] - a["early_losses"],
                core_delta=(None if a["first_core"] is None or b["first_core"] is None
                            else b["first_core"] - a["first_core"]),
                duration_delta=b["frame"] - a["frame"],
                win_delta=int(b["won"]) - int(a["won"]),
            ))
    return dict(
        schema="protodd-pvz-repeat-control-review-v1",
        plan_sha256=sha256(plan_path), review_source_sha256=sha256(__file__),
        trace_source_sha256=sha256(Path(__file__).with_name("pvz_core_transition_review.py")),
        original=original, repeat=repeat,
        checks=dict(same_inputs=same_inputs, paired=paired,
                    normal_runtime=original["healthy"] and repeat["healthy"] and
                    all(not row["errors"] and row["enemy_activity"]
                        for row in (*first.values(), *second.values()))),
        rows=rows, deterministic=bool(rows) and all(
            row["first_state_divergence"] is None and row["early_loss_delta"] == 0
            for row in rows),
        promotion_allowed=False,
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("plan", type=Path)
    parser.add_argument("original", type=Path)
    parser.add_argument("repeat", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.original, args.repeat)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({key: result[key] for key in ("checks", "rows", "deterministic")}))
