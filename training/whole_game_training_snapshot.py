"""Export an immutable diagnostic teacher from a completed stream group.

The source run keeps training. This snapshot is not a final checkpoint and may
only be used for interim disjoint-replay audits, never tournament deployment.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
from pathlib import Path

import torch

from .whole_game_fit import SCHEMA as FIT_SCHEMA


def snapshot(run_path, resume_path, output):
    run_path, resume_path, output = map(Path, (run_path, resume_path, output))
    if output.exists():
        raise FileExistsError(output)
    run_bytes = run_path.read_bytes()
    resume_bytes = resume_path.read_bytes()
    run = json.loads(run_bytes.decode("utf8"))
    digest = hashlib.sha256(json.dumps(run, sort_keys=True).encode()).hexdigest()
    resume = torch.load(io.BytesIO(resume_bytes), map_location="cpu", weights_only=True)
    if resume["spec_digest"] != digest:
        raise ValueError("stream checkpoint does not match its source-pinned run")
    completed = resume["next_group"]
    if not 0 < completed <= len(run["groups"]) * run["epochs"]:
        raise ValueError("checkpoint has no completed group or exceeds the run")
    output.mkdir(parents=True)
    teacher = dict(schema=FIT_SCHEMA, source_identity_sha256=run["source_identity_sha256"],
                   state_dict=resume["model"])
    torch.save(teacher, output / "teacher.pt")
    manifest = dict(schema="protodd-whole-game-diagnostic-snapshot-v1",
                    diagnostic_only=True, groups_completed=completed,
                    total_groups=len(run["groups"]) * run["epochs"],
                    source_run_sha256=hashlib.sha256(run_bytes).hexdigest(),
                    source_resume_sha256=hashlib.sha256(resume_bytes).hexdigest(),
                    source_identity_sha256=run["source_identity_sha256"],
                    teacher_sha256=hashlib.sha256((output / "teacher.pt").read_bytes()).hexdigest())
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n",
                                          encoding="utf8")
    return manifest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run", type=Path)
    parser.add_argument("resume", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    print(json.dumps(snapshot(args.run, args.resume, args.output), indent=2))


if __name__ == "__main__":
    main()
