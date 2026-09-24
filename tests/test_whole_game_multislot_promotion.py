from pathlib import Path
import hashlib
import json
import tempfile
import unittest
from unittest.mock import patch

from training.whole_game_multislot_promotion import candidate_traces, verify


class PromotionReceiptTest(unittest.TestCase):
    def test_repeated_early_mass_moves_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            received = Path(directory) / "server/replays/bot-write/game-0/Protodd/received"
            received.mkdir(parents=True)
            for name in ("WholeGame-controller.txt", "WholeGame-inference.csv",
                         "WholeGame-intents.csv"):
                (received / name).write_text("trace\n")
            actions = [
                f"ACTION,{frame},{actor},whole-game,Move,issued,accepted,target=-1,x=1536,y=2048"
                for frame in (7, 127, 247) for actor in (4, 6, 10, 15)]
            (received / "Protodd.log").write_text("\n".join(actions) + "\n")
            with self.assertRaisesRegex(ValueError, "repeated early mass moves"):
                candidate_traces(directory, [{"gameID": 0}])

    def test_receipt_rejects_missing_and_changed_evidence(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            weights = root / "weights.bin"
            weights.write_bytes(b"example weights")
            with self.assertRaisesRegex(ValueError, "not ready"):
                verify(self._receipt(root, weights, ready=False), weights)

            receipt = self._receipt(root, weights, ready=True)
            with self.assertRaisesRegex(ValueError, "incomplete"):
                verify(receipt, weights)

            data = json.loads(receipt.read_text())
            data["evidence"] = {
                name: {"path": str(root / name), "sha256": hashlib.sha256(b"ok").hexdigest()}
                for name in (
                    "package_manifest", "checkpoint", "run", "training_identity",
                    "validation_identity", "audit", "parity_early", "parity_mid",
                    "parity_late", "benchmark", "candidate_dll", "evaluation_build_record",
                    "resource_probe",
                    "candidate_manifest", "candidate_results", "baseline_manifest",
                    "baseline_results", "parity_probe")}
            for name in data["evidence"]:
                (root / name).write_bytes(b"ok")
            data["evidence"]["weights"] = dict(path=str(weights),
                                                 sha256=hashlib.sha256(weights.read_bytes()).hexdigest())
            receipt.write_text(json.dumps(data))
            with self.assertRaisesRegex(ValueError, "input list changed"):
                verify(receipt, weights)
            (root / "audit").write_bytes(b"tampered")
            with self.assertRaisesRegex(ValueError, "evidence changed: audit"):
                verify(receipt, weights)

    @staticmethod
    def _receipt(root, weights, *, ready):
        path = root / "receipt.json"
        path.write_text(json.dumps(dict(schema="protodd-whole-game-promotion-v1",
                                        tournament_ready=ready,
                                        weights_sha256=hashlib.sha256(weights.read_bytes()).hexdigest(),
                                        evidence={}, inputs={})))
        return path

    def test_rechecking_summary_cannot_be_skipped(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            weights = root / "weights.bin"
            weights.write_bytes(b"example weights")
            receipt = self._receipt(root, weights, ready=True)
            data = json.loads(receipt.read_text())
            names = (
                "package_manifest", "checkpoint", "run", "training_identity",
                "validation_identity", "audit", "parity_early", "parity_mid",
                "parity_late", "benchmark", "candidate_dll", "evaluation_build_record",
                "resource_probe",
                "candidate_manifest", "candidate_results", "baseline_manifest",
                "baseline_results", "parity_probe")
            for name in names:
                (root / name).write_bytes(b"ok")
            data["evidence"] = {
                name: dict(path=str(root / name), sha256=hashlib.sha256(b"ok").hexdigest())
                for name in names}
            data["evidence"]["weights"] = dict(
                path=str(weights), sha256=hashlib.sha256(weights.read_bytes()).hexdigest())
            data["inputs"] = {name: str(root) for name in (
                "package", "checkpoint", "training_release", "validation_release", "audit",
                "parity_early", "parity_mid", "parity_late", "benchmark", "candidate",
                "baseline", "dll", "evaluation_build_record", "resource_probe",
                "parity_probe")}
            data["summary"] = {"candidate_wins": 99}
            receipt.write_text(json.dumps(data))
            with patch("training.whole_game_multislot_promotion.evaluate",
                       return_value={"candidate_wins": 3}) as evaluation:
                with self.assertRaisesRegex(ValueError, "summary changed"):
                    verify(receipt, weights)
                evaluation.assert_called_once()


if __name__ == "__main__":
    unittest.main()
