"""Regression gates for silent replay corruption; game assets are never fixtures."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training import replay_assets
from training.playback_check import check_one, run_command


def pack(entries, body=b""):
    manifest = json.dumps(entries).encode()
    return struct.pack("<I", len(manifest)) + manifest + body


class PlaybackGates(unittest.TestCase):
    def test_rejects_truncated_duplicate_and_trailing_pack_data(self):
        invalid = [b"x", struct.pack("<I", 100) + b"[]",
                   pack([{"name": "a", "len": 4}], b"x"), pack([], b"extra"),
                   pack([{"name": "a", "len": 0}, {"name": "a", "len": 0}]),
                   pack([{"name": "a", "len": -1}])]
        for data in invalid:
            with self.subTest(data=data), self.assertRaises(ValueError):
                replay_assets.parse_pack(data)

    def test_wrong_pack_cannot_be_promoted(self):
        with tempfile.TemporaryDirectory() as temp:
            source, output = Path(temp) / "source", Path(temp) / "assets"
            source.write_bytes(pack([]))
            with self.assertRaisesRegex(ValueError, "pin"):
                replay_assets.prepare(source, output)
            self.assertFalse(output.exists())

    def test_missing_or_changed_terrain_cannot_fall_back(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp)
            manifest = {"version": 1, "pack_sha256": replay_assets.PACK_SHA256, "files": {}}
            for name, size in replay_assets.EXPECTED.items():
                data = b"x" * size
                path = output / name
                path.parent.mkdir(exist_ok=True)
                path.write_bytes(data)
                manifest["files"][name] = {"sha256": replay_assets.digest(data), "bytes": len(data)}
            (output / "manifest.json").write_text(json.dumps(manifest))
            replay_assets.verify(output)
            target = output / next(iter(replay_assets.EXPECTED))
            target.write_bytes(b"y" * target.stat().st_size)
            with self.assertRaisesRegex(ValueError, "changed"):
                replay_assets.verify(output)
            target.unlink()
            with self.assertRaises(FileNotFoundError):
                replay_assets.verify(output)

    def test_process_errors_and_timeouts_quarantine_replay(self):
        with tempfile.TemporaryDirectory() as temp:
            replay = Path(temp) / "input.rep"
            replay.write_bytes(b"replay-bytes")
            args = SimpleNamespace(output=Path(temp), decoder=Path("decoder"), timeout=1)
            for error in (ValueError("bad conversion"), subprocess.TimeoutExpired("decoder", 1)):
                with patch("training.playback_check.run_command", side_effect=error):
                    result = check_one(replay, args)
                self.assertEqual(result["status"], "quarantined")
                self.assertFalse(result["training_ready"])
                self.assertFalse(result["authoritative_game_validated"])
                self.assertIn("error", result)

    def test_nonzero_comparison_preserves_mismatch(self):
        result = subprocess.CompletedProcess([], 1, b'{"status":"quarantined","first_difference":{"frame":42}}', b"")
        with patch("training.playback_check.subprocess.run", return_value=result):
            self.assertEqual(json.loads(run_command(["node"], 1))["first_difference"]["frame"], 42)
        result.stdout = b"crashed"
        with patch("training.playback_check.subprocess.run", return_value=result):
            with self.assertRaises(ValueError):
                run_command(["node"], 1)


if __name__ == "__main__":
    unittest.main()
