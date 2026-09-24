"""Causal observation timeline with separate action and cadence timing targets.

Cadence windows supervise whether any confirmed action occurs in the next 24
frames. This is a label only: future commands never enter the observation tensor
or the recurrent memory. The last incomplete window is censored.
"""
from __future__ import annotations

from bisect import bisect_left
import json
from pathlib import Path

from .whole_game_labels import LABEL_SCHEMA
from .whole_game_pilot import SCHEMA


def trajectory_rows(summary, labels_stream, observations_stream, window=24):
    """Build causal targets from open streams; works for raw and gzip shards."""
    if (summary.get("schema") != SCHEMA or summary.get("complete") is not True
            or not 1 <= window <= 240):
        raise ValueError("incomplete or incompatible sequence source")
    labels = {}
    for line in labels_stream:
        label = json.loads(line)
        sequence = label["observation_sequence"]
        if label.get("schema") != LABEL_SCHEMA or sequence in labels:
            raise ValueError("duplicate or incompatible action label")
        labels[sequence] = label
    action_frames = sorted(label["frame"] for label in labels.values())
    previous_frame = -1
    previous_sequence = -1
    seen_labels = set()
    for line in observations_stream:
        row = json.loads(line)
        frame, sequence = row["frame"], row["sequence"]
        if (row["schema"] != SCHEMA or frame < previous_frame or
                sequence != previous_sequence + 1 or frame > summary["valid_through_frame"]):
            raise ValueError("noncausal observation timeline")
        previous_frame, previous_sequence = frame, sequence
        if row["reason"] == "cadence":
            complete = frame + window <= summary["valid_through_frame"]
            present = bisect_left(action_frames, frame + window) > bisect_left(action_frames, frame)
            yield row, dict(event=int(present) if complete else None, action=None,
                            update_memory=True)
        elif row["reason"] == "before_command":
            label = labels.get(sequence)
            if label is not None:
                if label["frame"] != frame or label["perspective"] != row["perspective"]:
                    raise ValueError("action bound to wrong observation")
                seen_labels.add(sequence)
            yield row, dict(event=None, action=label, update_memory=False)
        else:
            raise ValueError("unknown sampling reason")
    if seen_labels != labels.keys():
        raise ValueError("some action labels lack source observations")


def trajectory(extracted, labels_file, window=24):
    extracted, labels_file = Path(extracted), Path(labels_file)
    summary = json.loads((extracted / "summary.json").read_text())
    with labels_file.open() as labels, (extracted / "observations.jsonl").open() as observations:
        yield from trajectory_rows(summary, labels, observations, window)
