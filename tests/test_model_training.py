"""End-to-end synthetic training and numerical parity with the real C++ evaluator."""
import contextlib
import io
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from test_model_dataset import SCHEMA, write_fixture
from training.prepare import prepare
from training.model import MacroModel, export_model, load_model, masked_logits
from training.train import train

import numpy as np
import torch

TOOL = None


class TrainingTests(unittest.TestCase):
    def test_export_and_cpp_parity(self):
        if TOOL is None:
            self.skipTest("pass model_tool for C++ parity")
        torch.manual_seed(17)
        model = MacroModel((32, 24)).eval()
        rng = np.random.default_rng(12)
        features = rng.uniform(0, 2, size=(12, len(SCHEMA["features"]))).astype(np.float32)
        masks = rng.random((12, len(SCHEMA["actions"]))) > 0.4
        masks[:, 0] = True
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "model.bin"
            export_model(model, path)
            restored = load_model(path)
            with torch.no_grad():
                expected = model(torch.from_numpy(features))
                torch.testing.assert_close(expected, restored(torch.from_numpy(features)), rtol=0, atol=0)
                masked = masked_logits(expected, torch.from_numpy(masks))
                probability, actions = masked.softmax(1).max(1)
            rows = []
            for f, m in zip(features, masks):
                mask = sum((1 << i) for i, allowed in enumerate(m) if allowed)
                rows.append(str(mask) + " " + " ".join(format(float(x), ".9g") for x in f))
            result = subprocess.run([str(TOOL), "predict", str(path)], input="\n".join(rows) + "\n",
                                    capture_output=True, text=True, check=True)
            actual = [json.loads(line) for line in result.stdout.splitlines()]
            np.testing.assert_allclose([r["logits"] for r in actual], expected.numpy(), rtol=2e-5, atol=2e-6)
            np.testing.assert_allclose([r["probability"] for r in actual], probability.numpy(), rtol=2e-5, atol=2e-6)
            self.assertEqual([r["action"] for r in actual], actions.tolist())

    def test_training_excludes_test_and_exports_best(self):
        with tempfile.TemporaryDirectory() as directory:
            m, s = write_fixture(directory)
            dataset = Path(directory) / "data.sqlite"
            prepare(m, s, dataset, True)
            # Poison only final-test tensors. Training/selection must never touch them.
            with contextlib.closing(sqlite3.connect(dataset)) as db:
                db.execute("UPDATE samples SET features=x'' WHERE game_id='game-2'")
                db.commit()
            with contextlib.redirect_stdout(io.StringIO()):
                result = train(dataset, Path(directory) / "run", hidden=(16, 16), epochs=35,
                               batch_size=24, learning_rate=0.02, patience=8, seed=7, device="cpu")
            self.assertGreater(result["validation"]["all"]["top1"], 0.95)
            self.assertFalse(result["final_test_evaluated"])
            self.assertTrue(result["config"]["synthetic_only"])
            self.assertFalse(result["strength_validated"])
            self.assertEqual(result["deployment"], "shadow-only")
            load_model(Path(directory) / "run" / "LearnedMacro.bin")

    def test_invalid_models_and_masks(self):
        for widths in ((0, 16), (4096, 16), (True, 16)):
            with self.assertRaises(ValueError):
                MacroModel(widths)
        with self.assertRaises(ValueError):
            masked_logits(torch.zeros(2, 3), torch.zeros(2, 3, dtype=torch.bool))
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "bad.bin"
            p.write_bytes(b"PTDMLP1\n")
            with self.assertRaises(ValueError):
                load_model(p)
            model = MacroModel((4, 4))
            with torch.no_grad():
                next(model.parameters())[0, 0] = float("nan")
            with self.assertRaises(ValueError):
                export_model(model, p)


if __name__ == "__main__":
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        TOOL = Path(sys.argv.pop(1)).resolve()
    unittest.main()
