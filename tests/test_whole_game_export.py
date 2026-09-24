from pathlib import Path
import tempfile
import unittest

import torch

from training.whole_game_export import package, read_package
from training.whole_game_model import WholeGameModel


class WholeGameExportTests(unittest.TestCase):
    def test_all_model_parameters_round_trip_and_corruption_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / "teacher.pt"
            model = WholeGameModel(width=64, mixture_components=3)
            torch.save(dict(schema="protodd-whole-game-fit-v1",
                            source_identity_sha256="a" * 64,
                            state_dict=model.state_dict()), source)
            output = root / "export"
            manifest = package(source, output)
            loaded, state = read_package(output)
            self.assertEqual(manifest["weights_sha256"], loaded["weights_sha256"])
            self.assertEqual(set(state), set(model.state_dict()))
            for name, original in model.state_dict().items():
                self.assertTrue(torch.equal(state[name], original), name)
            binary = output / "weights.bin"
            raw = bytearray(binary.read_bytes())
            raw[-1] ^= 1
            binary.write_bytes(raw)
            with self.assertRaisesRegex(ValueError, "integrity"):
                read_package(output)


if __name__ == "__main__":
    unittest.main()
