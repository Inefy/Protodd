from __future__ import annotations

import json
import io
import sys
import tempfile
import unittest
from argparse import Namespace
from contextlib import redirect_stdout
from pathlib import Path
from unittest.mock import patch


TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))

import ladder  # noqa: E402


class LadderTests(unittest.TestCase):
    def test_proxy_java_requires_path_even_when_manager_has_java(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            launcher = Path(temporary) / "run_proxy.bat"
            for program in ("java", "java.exe", '"java.exe"'):
                with self.subTest(program=program):
                    launcher.write_text(f"@echo off\n{program} -jar ./bwapi-data/AI/bot.jar\n")
                    with patch.object(ladder.shutil, "which", return_value=None), self.assertRaisesRegex(
                        ladder.LadderError, "absolute Java path does not set PATH"
                    ):
                        ladder.validate_proxy_launcher("JavaBot", launcher)
                    with patch.object(ladder.shutil, "which", return_value="C:/Java/bin/java.exe"):
                        ladder.validate_proxy_launcher("JavaBot", launcher)

    def test_proxy_rejects_fixed_client_directory_even_if_it_exists(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            launcher = Path(temporary) / "run_proxy.bat"
            for change_directory in ('cd c:\\TM\\Starcraft\\bwapi-data\\AI\\',
                                     '@CD /D "C:\\TM\\Starcraft\\bwapi-data\\AI"',
                                     'chdir "D:/StarCraft/AI"'):
                with self.subTest(command=change_directory):
                    launcher.write_text(change_directory + "\nUAlbertaBot.exe\n")
                    with patch.object(Path, "is_dir", return_value=True), self.assertRaisesRegex(
                        ladder.LadderError, "hard-codes a client working directory"
                    ):
                        ladder.validate_proxy_launcher("UAlbertaBot", launcher)

    def test_proxy_accepts_portable_native_launcher_without_java(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            launcher = Path(temporary) / "run_proxy.bat"
            launcher.write_text('@echo off\nrem java is not needed\n:: java.exe\ncd /d "%~dp0"\nUAlbertaBot.exe\n')
            with patch.object(ladder.shutil, "which", return_value=None) as lookup:
                ladder.validate_proxy_launcher("UAlbertaBot", launcher)
            lookup.assert_not_called()

    def test_proxy_rejects_missing_absolute_executable(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            launcher = Path(temporary) / "run_proxy.bat"
            launcher.write_text('"C:\\missing runtime\\java.exe" -jar bot.jar\n')
            with patch.object(Path, "is_file", return_value=False), self.assertRaisesRegex(
                ladder.LadderError, "missing executable"
            ):
                ladder.validate_proxy_launcher("JavaBot", launcher)

    def test_prepare_rejects_broken_proxy_before_creating_snapshot(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            artifact = root / "Protodd.dll"
            artifact.write_bytes(b"unchanged protodd")
            ai = root / "bots" / "Proxy" / "AI"
            ai.mkdir(parents=True)
            (ai / "Proxy.dll").write_bytes(b"unchanged proxy")
            launcher = ai / "run_proxy.bat"
            config = root / "config.json"
            config.write_text(json.dumps({
                "our_bot": {"name": "Protodd", "artifact": "Protodd.dll"},
                "opponents": [{"name": "Proxy", "type": "proxy", "race": "Random",
                               "bwapi_version": "BWAPI_440", "directory": "bots/Proxy"}],
                "maps": ["Python.scx"], "rounds": 1,
            }))
            for script in ("java -jar bot.jar\n", "cd c:\\TM\\Starcraft\\bwapi-data\\AI\\\nUAlbertaBot.exe\n"):
                with self.subTest(script=script):
                    launcher.write_text(script)
                    before = {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()}
                    with patch.object(ladder, "REPO_ROOT", root), patch.object(
                        ladder.shutil, "which", return_value=None
                    ), patch.object(ladder.subprocess, "run") as process, self.assertRaises(ladder.LadderError):
                        ladder.command_prepare(Namespace(config=str(config), label="blocked"))
                    process.assert_not_called()
                    self.assertFalse((root / "ladder" / "runs").exists())
                    self.assertEqual(before, {p.relative_to(root): p.read_bytes() for p in root.rglob("*") if p.is_file()})

    def test_schedule_is_deterministic_and_balances_host(self) -> None:
        opponents = [{"name": "Iron"}, {"name": "Steamhammer"}]
        maps = ["maps/aiide/Benzene.scx", "maps/aiide/Python.scx"]
        first = ladder.make_schedule("Protodd", opponents, maps, 2)
        second = ladder.make_schedule("Protodd", opponents, maps, 2)

        self.assertEqual(first, second)
        self.assertEqual(len(first), 8)
        self.assertEqual([game["gameID"] for game in first], list(range(8)))
        self.assertEqual({game["map"] for game in first}, {"Benzene.scx", "Python.scx"})
        for opponent in ("Iron", "Steamhammer"):
            games = [game for game in first if opponent in (game["homeBot"], game["awayBot"])]
            self.assertEqual(sum(game["homeBot"] == "Protodd" for game in games), 2)
            self.assertEqual(sum(game["awayBot"] == "Protodd" for game in games), 2)

    def test_raw_tournament_reports_are_merged_and_failures_attributed(self) -> None:
        reports = [
            {
                "gameID": 0, "round": 0, "map": "Python", "reportingBot": "Protodd",
                "opponentBot": "Iron", "won": True, "crash": False, "gameEndType": "NORMAL",
                "gameTimeout": False, "finalFrame": 7200, "timers": [{"frameCount": 0}],
            },
            {
                "gameID": 0, "round": 0, "map": "Python", "reportingBot": "Iron",
                "opponentBot": "Protodd", "won": False, "crash": False, "gameEndType": "NORMAL",
                "gameTimeout": False, "finalFrame": 7200, "timers": [{"frameCount": 0}],
            },
            {
                "gameID": 1, "round": 1, "map": "Benzene", "reportingBot": "Protodd",
                "opponentBot": "Iron", "won": False, "crash": True,
                "gameEndType": "STARCRAFT_CRASH", "gameTimeout": False, "finalFrame": 2400,
                "timers": [{"frameCount": 0}],
            },
            {
                "gameID": 1, "round": 1, "map": "Benzene", "reportingBot": "Iron",
                "opponentBot": "Protodd", "won": True, "crash": False,
                "gameEndType": "NORMAL", "gameTimeout": False, "finalFrame": 2400,
                "timers": [{"frameCount": 0}],
            },
        ]
        records = ladder.merge_raw_reports(reports, "Protodd", [{"time_ms": 55, "frame_count": 320}])
        report = ladder.summarize(records, "Protodd")

        self.assertEqual(len(records), 2)
        self.assertTrue(records[0]["won"])
        self.assertFalse(records[1]["won"])
        self.assertTrue(records[1]["our_crash"])
        self.assertEqual(report["summary"]["wins"], 1)
        self.assertEqual(report["summary"]["our_crashes"], 1)

    def test_incomplete_reports_are_not_scored(self) -> None:
        reports = [
            {
                "gameID": 7, "round": 0, "map": "Python", "reportingBot": "Protodd",
                "opponentBot": "Iron", "won": True, "crash": False, "gameEndType": "NORMAL",
                "gameTimeout": False, "finalFrame": 100, "timers": [],
            }
        ]
        records = ladder.merge_raw_reports(reports, "Protodd", [])
        summary = ladder.summarize(records, "Protodd")["summary"]
        self.assertEqual(summary["scored_games"], 0)
        self.assertEqual(summary["excluded_incomplete_games"], 1)

    def test_detailed_results_javascript_is_supported(self) -> None:
        payload = [{
            "gameID": 2, "round": 0, "bots": ["Protodd", "Steamhammer"],
            "winner": 1, "crash": -1, "timeout": -1, "map": "Tau Cross",
            "gameEndType": "NORMAL", "duration": "00:05:00",
        }]
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "detailed_results_json.js"
            path.write_text("var replayPath='../';\nvar detailedResults = " + json.dumps(payload) + ";", encoding="utf-8")
            records = ladder.parse_results([path], "Protodd", [])
        self.assertEqual(records[0]["frames"], 7200)
        self.assertFalse(records[0]["won"])

    def test_difference_interval_flags_clear_improvement(self) -> None:
        delta, low, high = ladder.difference_interval(20, 100, 80, 100)
        self.assertAlmostEqual(delta, 0.6)
        self.assertGreater(low, 0.0)
        self.assertGreater(high, low)

    def test_prepare_creates_runnable_snapshot_and_manifest(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config_dir = root / "ladder"
            manager = config_dir / "manager"
            (manager / "server").mkdir(parents=True)
            (manager / "server" / "run_server.bat").write_text("@echo off\n", encoding="utf-8")
            artifact = root / "build" / "Protodd.dll"
            artifact.parent.mkdir()
            artifact.write_bytes(b"protodd")
            opponent = config_dir / "bots" / "Iron"
            (opponent / "AI").mkdir(parents=True)
            (opponent / "AI" / "Iron.dll").write_bytes(b"iron")
            (opponent / "read").mkdir()
            (opponent / "write").mkdir()
            maps = config_dir / "maps" / "maps.zip"
            maps.parent.mkdir()
            maps.write_bytes(b"maps")
            config = {
                "our_bot": {"name": "Protodd", "race": "Protoss", "type": "dll", "bwapi_version": "BWAPI_440", "artifact": "../build/Protodd.dll"},
                "opponents": [{"name": "Iron", "race": "Terran", "type": "dll", "bwapi_version": "BWAPI_412", "directory": "bots/Iron"}],
                "maps": ["maps/aiide/Python.scx"], "rounds": 2,
                "tournament_manager": "manager", "maps_archive": "maps/maps.zip",
                "timeout_limits": [{"time_ms": 55, "frame_count": 320}],
            }
            config_path = config_dir / "ladder.local.json"
            config_path.write_text(json.dumps(config), encoding="utf-8")

            with patch.object(ladder, "REPO_ROOT", root), patch.object(
                ladder, "git_output", side_effect=lambda *args: "abc123" if args[0] == "rev-parse" else ""
            ), redirect_stdout(io.StringIO()):
                ladder.command_prepare(Namespace(config=str(config_path), label="test-run"))

            run = config_dir / "runs" / "test-run"
            manifest = json.loads((run / "manifest.json").read_text(encoding="utf-8"))
            games = (run / "tournament" / "server" / "games.txt").read_text(encoding="utf-8").splitlines()
            self.assertEqual(manifest["git_commit"], "abc123")
            self.assertEqual(manifest["scheduled_games"], 2)
            self.assertEqual(len(games), 2)
            self.assertEqual(json.loads(games[0])["map"], "Python.scx")
            self.assertTrue((run / "tournament" / "server" / "bots" / "Protodd" / "AI" / "Protodd.dll").is_file())
            self.assertTrue((run / "tournament" / "server" / "bots" / "Iron" / "AI" / "Iron.dll").is_file())

    def test_report_writes_dashboard_and_per_game_telemetry(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            reports = [
                {"gameID": 0, "round": 0, "map": "Python", "reportingBot": "Protodd", "opponentBot": "Iron", "won": True, "crash": False, "gameEndType": "NORMAL", "gameTimeout": False, "finalFrame": 7200, "timers": []},
                {"gameID": 0, "round": 0, "map": "Python", "reportingBot": "Iron", "opponentBot": "Protodd", "won": False, "crash": False, "gameEndType": "NORMAL", "gameTimeout": False, "finalFrame": 7200, "timers": []},
            ]
            results = root / "results.txt"
            results.write_text("".join(json.dumps(item) + "\n" for item in reports), encoding="utf-8")
            manifest = root / "manifest.json"
            manifest.write_text(json.dumps({"our_bot": "Protodd", "label": "test", "scheduled_games": 1, "bots": [{"name": "Protodd", "race": "Protoss"}, {"name": "Iron", "race": "Terran"}]}), encoding="utf-8")
            log = root / "Protodd.log"
            log.write_text("START,Python,Iron,standard\nSTATE,3600,PvT,Attack,FastExpand,0.4,1.2,100,50,30,34\nPERF_SUMMARY,7200,0.8,2.0,0,0,0,0\nEND,win,7200\n", encoding="utf-8")
            output = root / "report"
            with redirect_stdout(io.StringIO()):
                ladder.command_report(Namespace(results=[str(results)], config=str(root / "missing.json"), manifest=str(manifest), our_bot=None, protodd_log=[str(log)], output=str(output)))

            self.assertTrue((output / "index.html").is_file())
            self.assertTrue((output / "games.csv").is_file())
            self.assertTrue((output / "telemetry-games.csv").is_file())
            parsed = json.loads((output / "report.json").read_text(encoding="utf-8"))
            self.assertEqual(parsed["summary"]["missing_scheduled_games"], 0)
            self.assertEqual(parsed["protodd_telemetry"]["runtime"]["over_55ms"], 0)


if __name__ == "__main__":
    unittest.main()
