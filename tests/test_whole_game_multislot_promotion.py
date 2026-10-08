from pathlib import Path
import hashlib
import json
import tempfile
import unittest
from unittest.mock import patch

from training.whole_game_contract import WEIGHTS_SCHEMA, model_contract
from training.whole_game_multislot_promotion import (
    EVIDENCE_NAMES,
    candidate_traces,
    make_evaluation_build_record,
    verify,
)


class PromotionReceiptTest(unittest.TestCase):
    def test_evaluation_record_binds_weight_schema_build_flags_and_scope(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dll = root / "ProtoddEvaluation.dll"
            image = bytearray(0x80)
            image[:2] = b"MZ"
            image[0x3c:0x40] = (0x40).to_bytes(4, "little")
            image[0x40:0x44] = b"PE\0\0"
            image[0x44:0x46] = (0x14c).to_bytes(2, "little")
            dll.write_bytes(image)
            weights = root / "weights.bin"
            weights.write_bytes(b"frozen six-slot fixture")
            (root / "manifest.json").write_text(json.dumps(dict(
                schema=WEIGHTS_SCHEMA, maximum_slots=6, tournament_ready=False,
                weights_sha256=hashlib.sha256(weights.read_bytes()).hexdigest(),
                model_contract=model_contract())))

            registry = json.loads((Path(__file__).resolve().parents[1] /
                                   "docs/feature-registry.json").read_text())
            values = {entry["id"]: entry["tournament_value"] for entry in registry["options"]}
            values.update({
                "PROTODD_WHOLE_GAME_CONTROL": "ON",
                "PROTODD_WHOLE_GAME_HYBRID": "ON",
                "PROTODD_WHOLE_GAME_EVALUATION_BUILD": "ON",
            })
            inputs = {entry["id"]: "" for entry in registry["build_inputs"]}
            inputs["PROTODD_WHOLE_GAME_WEIGHTS"] = str(weights.resolve())
            cache = root / "CMakeCache.txt"
            self._write_cmake_cache(cache, values, inputs)
            record = make_evaluation_build_record(dll, weights, cache)
            self.assertEqual(record["command_scope"], "hybrid-target-authority")
            self.assertEqual(record["weights_sha256"], hashlib.sha256(weights.read_bytes()).hexdigest())
            self.assertEqual(record["model_contract"], model_contract())
            self.assertEqual(record["build_options"], values)

            values["PROTODD_DEVELOPER_PROFILE"] = "ON"
            self._write_cmake_cache(cache, values, inputs)
            with self.assertRaisesRegex(ValueError, "unsafe command or research scope"):
                make_evaluation_build_record(dll, weights, cache)

    def test_promotion_receipt_rejects_a_changed_release_build_flag(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            weights = root / "weights.bin"
            weights.write_bytes(b"weights")
            registry = json.loads((Path(__file__).resolve().parents[1] /
                                   "docs/feature-registry.json").read_text())
            options = {entry["id"]: entry["tournament_value"] for entry in registry["options"]}
            options.update({"PROTODD_WHOLE_GAME_CONTROL": "ON",
                            "PROTODD_WHOLE_GAME_HYBRID": "ON",
                            "PROTODD_WHOLE_GAME_EVALUATION_BUILD": "ON"})
            inputs = {entry["id"]: "" for entry in registry["build_inputs"]}
            inputs["PROTODD_WHOLE_GAME_WEIGHTS"] = str(weights.resolve())
            receipt_path = root / "receipt.json"
            inputs["PROTODD_WHOLE_GAME_PROMOTION_RECEIPT"] = ""
            summary = dict(build_options=options, build_inputs=inputs)
            evidence = {}
            for name in (*EVIDENCE_NAMES, "weights", "parity_probe"):
                path = weights if name == "weights" else root / name
                if name != "weights":
                    path.write_bytes(name.encode())
                evidence[name] = dict(path=str(path.resolve()),
                                      sha256=hashlib.sha256(path.read_bytes()).hexdigest())
            receipt = dict(schema="protodd-whole-game-promotion-v1", tournament_ready=True,
                           weights_sha256=hashlib.sha256(weights.read_bytes()).hexdigest(),
                           summary=summary, evidence=evidence,
                           inputs={name: str(root) for name in (
                               "package", "checkpoint", "training_release", "validation_release",
                               "audit", "parity_early", "parity_mid", "parity_late", "benchmark",
                               "candidate", "baseline", "dll", "evaluation_build_record",
                               "resource_probe", "parity_probe")})
            evidence["weights"] = dict(path=str(weights.resolve()),
                                       sha256=hashlib.sha256(weights.read_bytes()).hexdigest())
            evidence["parity_probe"] = dict(path=str((root / "parity_probe").resolve()),
                                            sha256=hashlib.sha256((root / "parity_probe").read_bytes()).hexdigest())
            receipt_path.write_text(json.dumps(receipt))

            release_options = dict(options)
            release_options["PROTODD_WHOLE_GAME_EVALUATION_BUILD"] = "OFF"
            release_inputs = dict(inputs)
            release_inputs["PROTODD_WHOLE_GAME_PROMOTION_RECEIPT"] = str(receipt_path.resolve())
            cache = root / "release-CMakeCache.txt"
            self._write_cmake_cache(cache, release_options, release_inputs)
            with patch("training.whole_game_multislot_promotion.evaluate", return_value=summary):
                verify(receipt_path, weights, cache)

                release_options["PROTODD_PVZ_GATEWAY_OPENING"] = "ON"
                self._write_cmake_cache(cache, release_options, release_inputs)
                with self.assertRaisesRegex(ValueError, "release build flag differs"):
                    verify(receipt_path, weights, cache)

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

    @staticmethod
    def _write_cmake_cache(path, options, inputs):
        lines = [f"{name}:BOOL={value}" for name, value in options.items()]
        lines.extend(f"{name}:FILEPATH={value}" for name, value in inputs.items())
        path.write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    unittest.main()
