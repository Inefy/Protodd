import json
from pathlib import Path
import tempfile
import unittest
import zipfile

from training.arena import prepare, inspect, verify


class ArenaTests(unittest.TestCase):
    def fixture(self, root):
        template = root / "template"
        server = template / "server"
        (server / "required").mkdir(parents=True)
        (server / "html").mkdir()
        (server / "bots/Enemy/AI").mkdir(parents=True)
        (server / "bots/Enemy/AI/Enemy.dll").write_bytes(b"frozen opponent")
        (server / "bots/Enemy/read").mkdir()
        (server / "bots/Enemy/read/map-data.bin").write_bytes(b"required map data")
        for path in (server / "server.jar", server / "required/maps.zip"):
            with zipfile.ZipFile(path, "w") as z:
                z.writestr("maps/fixture.scx", "map")
        settings = dict(bots=[dict(BotName="Enemy", Race="Terran", BotType="dll", BWAPIVersion="BWAPI_440")],
                        maps=["maps/fixture.scx"], mapsFile="maps.zip",
                        tournamentModuleSettings=dict(timeoutLimits=[dict(timeInMS=55, frameCount=320)]))
        (server / "server_settings.json").write_text(json.dumps(settings))
        for n in (1, 2):
            client = template / f"client{n}"
            client.mkdir()
            runtime = root / f"runtime{n}"
            runtime.mkdir()
            (runtime / "StarCraft.exe").write_bytes(b"fixture")
            (client / "client_settings.json").write_text(json.dumps(dict(ClientStarcraftDir=str(runtime))))
            with zipfile.ZipFile(client / "client.jar", "w") as z:
                z.writestr("fixture", "fixture")
        dll = root / "candidate.dll"
        dll.write_bytes(b"candidate")
        return dict(template=template, output=root / "run", dll=dll, opponents=["Enemy"], maps=["maps/fixture.scx"])

    def test_frozen_purpose_hashes_and_both_host_sides(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            manifest = prepare(**args)
            self.assertEqual(manifest["games"], 2)
            run = args["output"]
            self.assertTrue(verify(run)["verified"])
            games = list(map(json.loads, (run / "server/games.jsonl").read_text().splitlines()))
            self.assertEqual([g["homeBot"] for g in games], ["Protodd", "Enemy"])
            self.assertEqual((run / "server/bots/Protodd/read/Policy-mode.txt").read_text(), "frozen\n")
            self.assertTrue((run / "server/bots/Enemy/read/map-data.bin").is_file())
            (run / "server/bots/Enemy/AI/Enemy.dll").write_bytes(b"changed")
            with self.assertRaises(ValueError):
                verify(run)

    def test_reject_placeholder_and_odd_pairing_before_creating_output(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            with self.assertRaises(ValueError):
                prepare(**args, rounds=3)
            (args["template"] / "client1/client.jar").write_bytes(b"placeholder")
            with self.assertRaises(ValueError):
                prepare(**args)
            self.assertFalse(args["output"].exists())

    def test_rewards_require_healthy_pairs_and_additional_activity_review(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            prepare(**args, purpose="training")
            run = args["output"]
            rows = [dict(gameID=0, reportingBot=a, opponentBot=b, map="fixture.scx", won=won,
                         gameEndType="NORMAL", crash=False, gameTimeout=False, finalFrame=12000,
                         timers=[dict(timeInMS=55, frameCount=0)])
                    for a, b, won in [("Protodd", "Enemy", True), ("Enemy", "Protodd", False)]]
            results = run / "server/results.jsonl"
            results.write_text(json.dumps(rows[0]) + "\n")
            self.assertEqual(len(inspect(run)["structurally_valid"]), 0)
            results.write_text("".join(json.dumps(r) + "\n" for r in rows))
            self.assertEqual(len(inspect(run)["structurally_valid"]), 1)
            self.assertFalse(inspect(run)["training_ready"])
            rows[1]["crash"] = True
            results.write_text("".join(json.dumps(r) + "\n" for r in rows))
            self.assertEqual(len(inspect(run)["structurally_valid"]), 0)


if __name__ == "__main__":
    unittest.main()
