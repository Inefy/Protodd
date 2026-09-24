"""Evaluate a train-only event-prior correction on disjoint cadence windows."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

from .whole_game_event_audit import MATCHUPS, summarize


SCHEMA = "protodd-whole-game-event-calibration-v1"


def corrected_probability(probability, logit_shift):
    if not 0 <= probability <= 1 or not math.isfinite(logit_shift):
        raise ValueError("invalid event probability or correction")
    bounded = min(1 - 1e-7, max(1e-7, probability))
    logit = math.log(bounded / (1 - bounded)) + logit_shift
    return 1 / (1 + math.exp(-logit))


def calibrate(event_report, training_prior, output):
    event_report, training_prior, output = map(Path, (event_report, training_prior, output))
    if output.exists():
        raise FileExistsError(output)
    event_bytes, prior_bytes = event_report.read_bytes(), training_prior.read_bytes()
    event = json.loads(event_bytes)
    prior = json.loads(prior_bytes)
    if (event["schema"] != "protodd-whole-game-event-audit-v1" or
            prior["schema"] != "protodd-whole-game-event-prior-v1" or
            prior["train_only"] is not True or
            prior["source_identity_sha256"] != event["training_identity_sha256"]):
        raise ValueError("event audit and train-only prior are incompatible")
    shift = prior["logit_prior_correction"]
    rows = [dict(row, probability=corrected_probability(row["probability"], shift))
            for row in event["rows"]]
    matchup_shifts = {}
    for matchup in MATCHUPS:
        counts = prior["by_matchup"][matchup]
        rate = counts["events"] / counts["windows"]
        if not 0 < rate < 1:
            raise ValueError("matchup timing prior lacks both event classes")
        matchup_shifts[matchup] = math.log(rate / (1 - rate))
    matchup_rows = [dict(row, probability=corrected_probability(
        row["probability"], matchup_shifts[row["matchup"]])) for row in event["rows"]]
    report = dict(schema=SCHEMA, train_only_calibration=True,
                  event_report_sha256=hashlib.sha256(event_bytes).hexdigest(),
                  train_prior_sha256=hashlib.sha256(prior_bytes).hexdigest(),
                  checkpoint_sha256=event["checkpoint_sha256"],
                  validation_identity_sha256=event["validation_identity_sha256"],
                  logit_prior_correction=shift,
                  raw=event["overall"], corrected=summarize(rows),
                  matchup_logit_prior_corrections=matchup_shifts,
                  matchup_corrected=summarize(matchup_rows),
                  by_matchup={matchup: summarize([row for row in rows
                                                   if row["matchup"] == matchup])
                              for matchup in MATCHUPS},
                  by_matchup_with_matchup_prior={
                      matchup: summarize([row for row in matchup_rows
                                          if row["matchup"] == matchup])
                      for matchup in MATCHUPS},
                  strength_validated=False)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("event_report", type=Path)
    parser.add_argument("training_prior", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    report = calibrate(args.event_report, args.training_prior, args.output)
    print(json.dumps(dict(raw=report["raw"], corrected=report["corrected"]), indent=2))


if __name__ == "__main__":
    main()
