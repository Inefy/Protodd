"""Check the selected model's real validation inputs in Python and the C++ runtime."""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np
import torch

from training.dataset import batches, metadata
from training.model import load_model, masked_logits
from training.schema import sha256


def verify_export(dataset, model, tool, output):
    metadata(dataset)
    features, masks, *_ = next(batches(dataset, "validation", 128))
    restored = load_model(model).eval()
    with torch.no_grad():
        expected = restored(features)
        probabilities, actions = masked_logits(expected, masks).softmax(1).max(1)
    lines = []
    for values, allowed in zip(features.tolist(), masks.tolist()):
        mask = sum(1 << i for i, value in enumerate(allowed) if value)
        lines.append(str(mask) + " " + " ".join(format(v, ".9g") for v in values))
    result = subprocess.run([str(tool), "predict", str(model)], input="\n".join(lines) + "\n",
                            text=True, capture_output=True, check=True, timeout=120)
    actual = [json.loads(line) for line in result.stdout.splitlines()]
    np.testing.assert_allclose([r["logits"] for r in actual], expected.numpy(), rtol=2e-5, atol=1e-4)
    np.testing.assert_allclose([r["probability"] for r in actual], probabilities.numpy(), rtol=2e-5, atol=1e-5)
    if [r["action"] for r in actual] != actions.tolist():
        raise ValueError("Exported C++ model selected different actions")
    report = {"passed": True, "samples": len(actual), "split": "validation",
              "model_sha256": sha256(model), "runtime_sha256": sha256(tool),
              "max_logit_error": float(np.abs(np.asarray([r["logits"] for r in actual]) - expected.numpy()).max()),
              "deployment": "shadow-only", "strength_validated": False}
    Path(output).write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("dataset", "model", "tool", "output"):
        parser.add_argument("--" + name, type=Path, required=True)
    print(json.dumps(verify_export(**vars(parser.parse_args()))))
