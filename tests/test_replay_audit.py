"""The raw replay audit must never certify playback or transfer player quality."""
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.replay_audit import audit_one, digest, summarize
from training.prepare import validate_manifest
from training.schema import load_schema


def fixture():
    return {"Header": {"Version": "1.21+", "Frames": 100, "StartTime": "2026-09-01T00:00:00Z",
                       "Map": "Test", "Engine": {"ID": 1}, "Speed": {"ID": 6}, "Type": {"ID": 15},
                       "Players": [{"Name": "terran", "ID": 0, "SlotID": 2, "Type": {"ID": 2},
                                    "Race": {"ID": 1}, "Team": 1},
                                   {"Name": "protoss", "ID": 1, "SlotID": 5, "Type": {"ID": 2},
                                    "Race": {"ID": 2}, "Team": 2}]},
            "Commands": {"Cmds": [{"PlayerID": 1, "Frame": 3, "Type": {"ID": 31, "Name": "Train"},
                                   "Unit": {"ID": 64}}], "ParseErrCmds": None},
            "Custom": {"MapDataHash": "a" * 64}, "Limits": {"units": 3400}}


class AuditTests(unittest.TestCase):
    def test_parser_success_is_never_training_readiness(self):
        result = summarize(fixture(), {}, "TvP")
        self.assertEqual(result["parse_status"], "parsed")
        self.assertFalse(result["training_ready"])
        self.assertIn("modern_replay_requires_compatible_playback", result["training_blockers"])
        self.assertIn("unit_limit_exceeds_legacy_engine", result["training_blockers"])
        with self.assertRaises(ValueError):
            validate_manifest(result, load_schema())

    def test_quality_attaches_to_exact_player_only(self):
        source = {"toon": "terran", "race": "T", "mmr": 2600,
                  "opponentToon": "protoss", "opponentRace": "P", "opponentProId": "claim"}
        players = summarize(fixture(), source, "TvP")["players"]
        self.assertEqual(players[0]["source_claims"][0]["mmr_claim"], 2600)
        protoss = players[1]["source_claims"][0]
        self.assertEqual(protoss["pro_id_claim"], "claim")
        self.assertIsNone(protoss["mmr_claim"])
        self.assertFalse(protoss["identity_verified"])
        source["opponentToon"] = "different-name"
        self.assertEqual(summarize(fixture(), source, "TvP")["players"][1]["source_claims"], [])

    def test_pvp_duplicate_name_is_ambiguous(self):
        parsed = fixture()
        for p in parsed["Header"]["Players"]:
            p.update(Name="same", Race={"ID": 2})
        result = summarize(parsed, {"toon": "same", "race": "P", "proId": "claim"}, "PvP")
        self.assertTrue(all(not p["source_claims"] for p in result["players"]))

    def test_partial_parser_success_quarantines_commands(self):
        parsed = fixture()
        parsed["Commands"]["ParseErrCmds"] = [{"Frame": 4, "Type": {"ID": 255}}]
        result = summarize(parsed, {}, "TvP")
        self.assertEqual(result["parse_status"], "quarantined")
        self.assertIn("command_parse_errors", result["quarantine_reasons"])

    def test_wrong_matchup_and_bad_timing_are_quarantined(self):
        parsed = fixture()
        parsed["Commands"]["Cmds"][0]["Frame"] = 101
        result = summarize(parsed, {}, "PvP")
        self.assertIn("catalog_matchup_mismatch", result["quarantine_reasons"])
        self.assertIn("invalid_command_timing", result["quarantine_reasons"])

    def test_observer_does_not_create_third_player(self):
        parsed = fixture()
        observer = copy.deepcopy(parsed["Header"]["Players"][0])
        observer.update(ID=2, Observer=True, Name="observer")
        parsed["Header"]["Players"].append(observer)
        self.assertEqual(len(summarize(parsed, {}, "TvP")["players"]), 2)

    def test_parser_receives_the_exact_hashed_bytes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            path = root / "TvP" / "test.rep"
            path.parent.mkdir()
            path.write_bytes(b"original replay content")
            response = subprocess.CompletedProcess([], 0, json.dumps(fixture()).encode(), b"")
            with patch("training.replay_audit.subprocess.run", return_value=response) as parser:
                result = audit_one(path, root, {}, Path("screp"), 30)
            self.assertEqual(digest(parser.call_args.kwargs["input"]), result["sha256"])
            self.assertEqual(result["parse_status"], "parsed")
            with patch("training.replay_audit.subprocess.run", side_effect=subprocess.TimeoutExpired([], 30)):
                result = audit_one(path, root, {}, Path("screp"), 30)
            self.assertEqual(result["parse_status"], "failed")
            self.assertFalse(result["training_ready"])


if __name__ == "__main__":
    unittest.main()
