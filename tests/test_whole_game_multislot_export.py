from pathlib import Path
import json
import subprocess
import tempfile
import unittest

import numpy as np
import torch

from training.whole_game_cpu_parity import read_output, write_input
from training.whole_game_export import read_package
from training.whole_game_multislot_export import package
from training.whole_game_multislot_model import MultiSlotWholeGameModel


ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "build/whole-game-cpu-probe/Release/whole_game_cpu_probe.exe"


def observation():
    sample = dict(type=torch.tensor([[64, 65]]),
                  relation=torch.tensor([[0, 1]]),
                  order=torch.zeros((1, 2), dtype=torch.long),
                  entity_numeric=torch.zeros((1, 2, 16)),
                  entity_mask=torch.ones((1, 2), dtype=torch.bool),
                  spatial=torch.zeros((1, 3, 8, 8)),
                  global_=torch.zeros((1, 216)))
    sample["global"] = sample.pop("global_")
    return sample


class MultiSlotExportTest(unittest.TestCase):
    def test_versioned_package_and_compiled_autoregressive_parity(self):
        torch.set_num_threads(1)
        torch.manual_seed(17)
        model = MultiSlotWholeGameModel(width=64, mixture_components=3).eval()
        with torch.no_grad():
            model.slot_stop.bias[:] = torch.tensor([10.0, -10.0])
            model.slot_kind.bias[0] = 10.0
            model.slot_mode.bias[2] = 10.0
            model.slot_delay.bias[0] = 10.0
            model.slot_arguments["unit_type"].bias[65] = 10.0
        sample = observation()
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            checkpoint = root / "teacher.pt"
            torch.save(dict(schema="protodd-whole-game-multislot-fit-v1",
                            source_identity_sha256="a" * 64,
                            state_dict=model.state_dict()), checkpoint)
            (root / "run.json").write_text(json.dumps(dict(
                schema="protodd-whole-game-multislot-fit-v1",
                maximum_slots=6, training_identity_sha256="a" * 64)))
            manifest = package(checkpoint, root / "package")
            loaded, state = read_package(root / "package")
            self.assertEqual(loaded["schema"], manifest["schema"])
            self.assertEqual(set(state), set(model.state_dict()))
            self.assertEqual(manifest["maximum_slots"], 6)
            if not PROBE.exists():
                self.skipTest("compiled CPU probe has not been built")
            fixture, output = root / "input.bin", root / "output.bin"
            write_input(fixture, sample, None)
            subprocess.run([str(PROBE), str(root / "package/weights.bin"),
                            str(fixture), str(output)], check=True,
                           capture_output=True, text=True, timeout=120)
            actual = read_output(output)
            with torch.inference_mode():
                expected = model.forward_slots(sample)
            self.assertEqual(int(actual["slot_count"][0]), 6)
            np.testing.assert_allclose(actual["memory"],
                                       expected["backbone"]["memory"].numpy().reshape(-1),
                                       atol=3e-4, rtol=3e-4)
            for index, slot in enumerate(expected["slots"]):
                for name in ("stop", "delay", "position", "domain", "queued",
                             "order", "unit_type", "technology", "upgrade", "queue_slot"):
                    np.testing.assert_allclose(actual[f"slot{index}.{name}"],
                                               slot[name].numpy().reshape(-1),
                                               atol=3e-4, rtol=3e-4,
                                               err_msg=f"slot {index} head {name}")


if __name__ == "__main__":
    unittest.main()
