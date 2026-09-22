"""Check real replay extraction against validated checkpoints and accepted-command traces."""
import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import subprocess
import tempfile

from training.playback_check import run_command
from training.replay_pipeline import checkpoints_hash
from training.schema import load_schema, sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("report", "decoder", "extractor", "selftest", "mpq", "assets", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists():
        raise ValueError("Audit output already exists")
    reference = json.loads(args.report.read_text())
    schema = load_schema()
    indexes = {f["name"]: i for i, f in enumerate(schema["features"])}
    records = []
    integration_passed = False
    for number, replay in enumerate(reference["records"]):
        with tempfile.TemporaryDirectory(prefix="extract-audit-", dir=args.output.parent) as directory:
            temp = Path(directory)
            path = Path(replay["replay"])
            if sha256(path) != replay["sha256"]:
                raise ValueError("Replay changed")
            raw = temp / "replay.raw"
            converted = json.loads(run_command([str(args.decoder), str(path), str(raw)], 120))
            if converted["decoded_sha256"] != replay["conversion"]["decoded_sha256"]:
                raise ValueError("Decoder differs from validation")
            if not integration_passed and path.parent.name == "PvP":
                run_command([str(args.selftest), str(args.mpq), str(args.assets), str(raw)], 120)
                integration_passed = True
            samples, actions, checkpoints = [temp / n for n in ("samples.jsonl", "actions.jsonl", "checkpoints.jsonl")]
            stats = json.loads(run_command([str(args.extractor), str(args.mpq), str(args.assets), str(raw),
                "audit:" + replay["sha256"], str(samples), str(actions), str(checkpoints)], 180))
            if stats["fingerprint"] != schema["fingerprint"] or stats["end_frame"] != replay["end_frame"]:
                raise ValueError("Wrong schema or end frame")
            if checkpoints_hash(checkpoints) != replay["native_checkpoints_sha256"]:
                raise ValueError("Extractor command instrumentation changed replay state")
            accepted = defaultdict(list)
            for line in actions.read_text().splitlines():
                action = json.loads(line)
                if action["accepted"]:
                    if action["producer"] < 0 or action["repeated"]:
                        raise ValueError("Invalid accepted producer/duplicate")
                    accepted[(action["owner"], action["frame"] // 24 * 24)].append(action)
            states = {s["frame"]: s for s in map(json.loads, checkpoints.read_text().splitlines())}
            slots = {p["perspective"]: p["slot"] for p in stats["perspectives"]}
            count = positive = 0
            last = {}
            with samples.open() as stream:
                for line in stream:
                    row = json.loads(line)
                    f = row["frame"]
                    perspective = row["perspective"]
                    if (f % 24 or f <= last.get(perspective, -1) or not f <= row["action_frame"] < f+24
                            or len(row["features"]) != len(schema["features"])
                            or any(not math.isfinite(v) or not 0 <= v <= 16 for v in row["features"])
                            or row["action"] not in row["allowed_actions"]):
                        raise ValueError("Malformed/illegal extraction row")
                    last[perspective] = f
                    labels = accepted[(slots[perspective], f)]
                    expected = labels[0]["action"] if labels else "wait"
                    if row["action"] != expected or (labels and row["action_frame"] != labels[0]["frame"]):
                        raise ValueError("Sample does not match first accepted request in its window")
                    if f in states:
                        economy = states[f]["players"][slots[perspective]]
                        checks = {"minerals": economy["minerals"] / 2000, "gas": economy["gas"] / 2000,
                            "supply_used_doubled": economy["usedSupplyRaw"][2] / 400,
                            "supply_total_doubled": min(400, economy["maxSupplyRaw"][2]) / 400}
                        if any(abs(row["features"][indexes[k]] - min(16, max(0, v))) > 2e-6 for k,v in checks.items()):
                            raise ValueError("Observation economy does not match pre-action checkpoint")
                    count += 1; positive += row["action"] != "wait"
            records.append({"sha256": replay["sha256"], "samples": count, "positive_samples": positive,
                            "stats": stats, "passed": True})
            print(f"{number+1}/{len(reference['records'])}: {count} legal rows, {positive} positive labels", flush=True)
    evidence = {"passed": bool(records) and integration_passed,
        "extractor_sha256": sha256(args.extractor), "selftest_sha256": sha256(args.selftest),
        "schema": schema["version"], "fingerprint": schema["fingerprint"],
        "reference_report_sha256": sha256(args.report), "replays": len(records),
        "samples": sum(r["samples"] for r in records), "positive_samples": sum(r["positive_samples"] for r in records),
        "perspective_audit": "native hidden-state perturbation, fog/detection; shared memory tests run in CTest",
        "action_audit": "native rejected/accepted/repeated commands; exact first-accepted label alignment for every audited row",
        "authoritative_game_validated": False, "records": records}
    args.output.write_text(json.dumps(evidence, indent=2) + "\n")


if __name__ == "__main__":
    main()
