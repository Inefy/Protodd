import json
import tempfile
import unittest
from pathlib import Path

from training.whole_game_controller_audit import summarize


class ControllerAuditTests(unittest.TestCase):
    def test_paired_health_requires_actual_learned_commands(self):
        with tempfile.TemporaryDirectory() as temporary:
            campaign = Path(temporary)
            (campaign / "manifest.json").write_text(json.dumps({"bot": "Protodd"}))
            server = campaign / "server"
            server.mkdir()
            (server / "games.jsonl").write_text('{"gameID": 0}\n')
            own = dict(gameID=0, reportingBot="Protodd", gameEndType="NORMAL",
                       won=True, crash=False, gameTimeout=False,
                       timers=[dict(timeInMS=55, frameCount=0)])
            other = dict(own, reportingBot="Opponent", won=False)
            (server / "results.jsonl").write_text(json.dumps(own) + "\n" + json.dumps(other) + "\n")
            received = server / "replays/bot-write/game-0/Protodd/received"
            received.mkdir(parents=True)
            (received / "WholeGame-controller.txt").write_text("compiled-control\n")
            (received / "WholeGame-inference.csv").write_text("7,12,0,14.2,1,2\n")
            log = received / "Protodd.log"
            log.write_text("ACTION_TOTAL,240,whole-game,issued,accepted,2\n"
                           "PERF_SUMMARY,241,0.4,25.5,0,0,0,0\n")
            report = summarize(campaign)
            self.assertTrue(report["controller_operational"])
            self.assertFalse(report["strength_validated"])
            self.assertEqual(report["games"][0]["learned_commands_accepted"], 2)
            log.write_text("PERF_SUMMARY,241,0.4,25.5,0,0,0,0\n")
            self.assertFalse(summarize(campaign)["controller_operational"])
            (received / "WholeGame-controller.txt").unlink()
            self.assertFalse(summarize(campaign)["games"][0]["complete"])


if __name__ == "__main__":
    unittest.main()
