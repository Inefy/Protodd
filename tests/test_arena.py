import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
import zipfile

from training.arena import prepare, inspect, verify, feature_registry_sha256, read_feature_registry
from training.schema import sha256


class ArenaTests(unittest.TestCase):
    def test_missing_campaign_template_is_an_explicit_non_start(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            args = self.fixture(root / "absent-template")
            args["template"] = root / "absent-template" / "missing-template"
            with self.assertRaisesRegex(ValueError, "campaign template is missing"):
                prepare(**args)
            self.assertFalse(args["output"].exists())

            args = self.fixture(root / "missing-settings")
            (args["template"] / "server/server_settings.json").unlink()
            with self.assertRaisesRegex(ValueError, "campaign template lacks server settings"):
                prepare(**args)
            self.assertFalse(args["output"].exists())

    def test_allin_profile_is_development_only_and_frozen(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp), {"PROTODD_NATIVE_ALLIN_OPENING": "ON"})
            for opening, purpose in [('bad', 'development')]:
                with self.assertRaisesRegex(ValueError, 'all-in opening requires'):
                    prepare(**args, purpose=purpose, all_in_opening=opening)
                self.assertFalse(args['output'].exists())
            safe_args = self.fixture(Path(temp) / 'safe-release')
            with self.assertRaisesRegex(ValueError, 'all-in opening requires'):
                prepare(**safe_args, purpose='final-test', all_in_opening='two-gate-zealot')
            prepare(**args, all_in_opening='two-gate-zealot')
            path = args['output'] / 'server/bots/Protodd/read/AllIn-opening.txt'
            self.assertEqual(path.read_text(), 'two-gate-zealot\n')
            self.assertTrue(verify(args['output'])['verified'])
            path.write_text('dt-pressure\n')
            with self.assertRaises(ValueError):
                verify(args['output'])

    def test_hybrid_authority_is_development_only_and_frozen(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp), {
                "PROTODD_WHOLE_GAME_CONTROL": "ON",
                "PROTODD_WHOLE_GAME_EVALUATION_BUILD": "ON",
            })
            for mode, purpose in [('invalid', 'development'), ('shadow', 'training')]:
                with self.assertRaisesRegex(ValueError, 'hybrid comparison requires'):
                    prepare(**args, purpose=purpose, whole_game_hybrid_mode=mode)
                self.assertFalse(args['output'].exists())
            safe_args = self.fixture(Path(temp) / 'safe-release')
            with self.assertRaisesRegex(ValueError, 'hybrid comparison requires'):
                prepare(**safe_args, purpose='final-test', whole_game_hybrid_mode='shadow')
            prepare(**args, whole_game_hybrid_mode='shadow')
            path = args['output'] / 'server/bots/Protodd/read/WholeGame-hybrid-mode.txt'
            self.assertEqual(path.read_text(), 'shadow\n')
            self.assertTrue(verify(args['output'])['verified'])
            path.write_text('target\n')
            with self.assertRaises(ValueError):
                verify(args['output'])

    def test_policy_off_is_development_only_and_pinned(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            with self.assertRaisesRegex(ValueError, 'policy-off comparison requires'):
                prepare(**args, purpose='final-test', policy_mode='off')
            self.assertFalse(args['output'].exists())
            prepare(**args, policy_mode='off')
            path = args['output'] / 'server/bots/Protodd/read/Policy-mode.txt'
            self.assertEqual(path.read_text(), 'off\n')
            self.assertTrue(verify(args['output'])['verified'])
            path.write_text('frozen\n')
            with self.assertRaises(ValueError):
                verify(args['output'])

    def test_development_slow_frame_allowance_is_frozen(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            with self.assertRaisesRegex(ValueError, 'slow-frame allowance requires'):
                prepare(**args, purpose='final-test', slow_frame_allowance=640)
            self.assertFalse(args['output'].exists())
            prepare(**args, slow_frame_allowance=640)
            settings = args['output'] / 'server/server_settings.json'
            self.assertEqual(json.loads(settings.read_text())['tournamentModuleSettings']
                             ['timeoutLimits'][0]['frameCount'], 640)
            self.assertTrue(verify(args['output'])['verified'])
            value = json.loads(settings.read_text())
            value['tournamentModuleSettings']['timeoutLimits'][0]['frameCount'] = 320
            settings.write_text(json.dumps(value))
            with self.assertRaises(ValueError):
                verify(args['output'])

    def test_worker_intervention_is_training_only_and_frozen(self):
        with tempfile.TemporaryDirectory() as temp:
            args=self.fixture(Path(temp), {"PROTODD_PRODUCTION_LOCAL_EVALUATION": "ON"})
            with self.assertRaisesRegex(ValueError,'worker interventions require'):
                prepare(**args,worker_training_intervention='plus-one')
            self.assertFalse(args['output'].exists())
            prepare(**args,purpose='training',worker_training_intervention='plus-one')
            read=args['output']/'server/bots/Protodd/read'
            self.assertEqual((read/'WorkerTraining-mode.txt').read_text(),'plus-one\n')
            self.assertEqual((read/'Protodd-learning-mode.txt').read_text(),'frozen\n')
            self.assertEqual((read/'Policy-mode.txt').read_text(),'frozen\n')
            self.assertTrue(verify(args['output'])['verified'])
            (read/'WorkerTraining-mode.txt').write_text('plus-two\n')
            with self.assertRaises(ValueError):verify(args['output'])

    def test_failed_feedback_cannot_prepare_control(self):
        with tempfile.TemporaryDirectory() as temp:
            args=self.fixture(Path(temp));model=Path(temp)/'model.bin';model.write_bytes(b'frozen')
            receipt=Path(temp)/'receipt.json';receipt.write_text(json.dumps({'shadow_gate_pass':True,'feedback_gate_pass':False}))
            with self.assertRaisesRegex(ValueError,'passing shadow'):
                prepare(**args,production_shadow=model,frame_limit=7200,production_control_receipt=receipt)
            self.assertFalse(args['output'].exists())

    def test_production_shadow_is_pinned_and_does_not_enable_control(self):
        with tempfile.TemporaryDirectory() as temp:
            args=self.fixture(Path(temp))
            model=Path(temp)/'demand.bin';model.write_bytes(b'frozen weights')
            prepare(**args,production_shadow=model)
            read=args['output']/'server/bots/Protodd/read'
            self.assertEqual((read/'ProductionDemand-mode.txt').read_text(),'shadow\n')
            self.assertEqual((read/'LearnedMacro-mode.txt').read_text(),'off\n')
            self.assertTrue(verify(args['output'])['verified'])
            (read/'ProductionDemand.bin').write_bytes(b'changed')
            with self.assertRaises(ValueError):verify(args['output'])

    def test_tactical_target_weights_are_frozen_local_inputs(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp), {"PROTODD_TACTICAL_LOCAL_EVALUATION": "ON"})
            weights = Path(temp) / 'target.bin'
            weights.write_bytes(b'frozen target weights')
            with self.assertRaisesRegex(ValueError, 'tactical target control needs'):
                prepare(**args, purpose='training', tactical_target_weights=weights)
            self.assertFalse(args['output'].exists())
            prepare(**args, tactical_target_weights=weights)
            read = args['output'] / 'server/bots/Protodd/read'
            self.assertEqual((read / 'TacticalTarget-weights.bin').read_bytes(),
                             weights.read_bytes())
            self.assertEqual((read / 'TacticalTarget-mode.txt').read_text(),
                             'local-target\n')
            self.assertTrue(verify(args['output'])['verified'])
            (read / 'TacticalTarget-mode.txt').write_text('off\n')
            with self.assertRaises(ValueError):
                verify(args['output'])

    @unittest.skipUnless(os.name == 'nt' and shutil.which('pwsh'), 'Windows process ownership fixture')
    def test_cleanup_does_not_kill_peer_or_unknown_process(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            runtime = root / 'runtime'
            runtime.mkdir()
            (runtime / 'StarCraft.exe').write_bytes(b'fixture')
            (runtime / 'arena-owned-processes.json').write_text(json.dumps(dict(runtime=str(runtime), processes=[
                dict(pid=4, parent=40, created='2026-09-22T12:00:00Z'),
                dict(pid=5, parent=50, created='2026-09-21T12:00:00Z')])))
            # Mock OS enumeration/termination only; execute the actual helper.
            harness = root / 'ownership.ps1'
            harness.write_text('''param($Helper, $Runtime)
$global:arenaStopped = @()
function Get-CimInstance {
    [PSCustomObject]@{ProcessId=1; ExecutablePath=(Join-Path $Runtime 'StarCraft.exe')}
    [PSCustomObject]@{ProcessId=2; ExecutablePath=(Join-Path (Split-Path $Runtime) 'peer/StarCraft.exe')}
    [PSCustomObject]@{ProcessId=3; ExecutablePath=$null}
    [PSCustomObject]@{ProcessId=4; ParentProcessId=40; CreationDate=[DateTime]::Parse('2026-09-22T12:00:00Z'); ExecutablePath=$null}
    [PSCustomObject]@{ProcessId=5; ParentProcessId=50; CreationDate=[DateTime]::Parse('2026-09-22T12:00:00Z'); ExecutablePath=$null}
}
function Stop-Process { param($Id, [switch]$Force, $ErrorAction) $global:arenaStopped += $Id }
& $Helper -Runtime $Runtime
if (($global:arenaStopped -join ',') -ne '1,4') { throw 'peer ownership or PID-reuse protection violated' }
''')
            helper = Path(__file__).resolve().parents[1] / 'scripts/stop-owned-starcraft.ps1'
            result = subprocess.run(['pwsh', '-NoProfile', '-File', str(harness), str(helper), str(runtime)],
                                    capture_output=True, text=True, timeout=20)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_client_bundle_is_pinned_with_ownership_helper(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            bundle = Path(temp) / 'bundle'
            bundle.mkdir()
            shutil.copy2(args['template'] / 'client1/client.jar', bundle / 'client.jar')
            (bundle / 'stop-owned-starcraft.ps1').write_text('fixture')
            (bundle / 'start-owned-starcraft.ps1').write_text('fixture')
            for helper in ('HeadlessStarCraft.psm1', 'HeadlessStarCraft.cs', 'run-headless-starcraft.ps1',
                           'MatchProcessOwnership.psm1'):
                (bundle / helper).write_text('fixture')
            prepare(**args, client_bundle=bundle)
            self.assertTrue(verify(args['output'])['verified'])
            (args['output'] / 'client1/stop-owned-starcraft.ps1').write_text('changed')
            with self.assertRaises(ValueError):
                verify(args['output'])

    def fixture(self, root, build_overrides=None):
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
        registry = read_feature_registry()
        overrides = build_overrides or {}
        active_options = [
            dict(id=entry["id"], value=overrides.get(entry["id"], entry["tournament_value"]),
                 tournament_value=entry["tournament_value"], owner=entry["owner"], scope=entry["scope"],
                 baseline=entry["baseline"], dependencies=entry["dependencies"],
                 evidence=entry["evidence"], promotion_status=entry["promotion_status"])
            for entry in registry["options"]
        ]
        active_inputs = [dict(id=entry["id"], value=entry["tournament_value"],
                              tournament_value=entry["tournament_value"], owner=entry["owner"],
                              scope=entry["scope"], baseline=entry["baseline"],
                              dependencies=entry["dependencies"], evidence=entry["evidence"],
                              promotion_status=entry["promotion_status"])
                         for entry in registry["build_inputs"]]
        runtime_contract = [dict(
            id=entry["id"], file=entry["file"], kind=entry["kind"],
            default_value=entry["default_value"], allowed_values=entry["allowed_values"],
            tournament_values=entry["tournament_values"], dependencies=entry["dependencies"],
            scope=entry["scope"], owner=entry["owner"], baseline=entry["baseline"],
            evidence=entry["evidence"], promotion_status=entry["promotion_status"])
            for entry in registry["runtime_controls"]]
        registry_hash = feature_registry_sha256()
        build_manifest = dict(
            schema="protodd-build-v1", feature_registry_sha256=registry_hash,
            dll_sha256=sha256(dll), registered_feature_manifest=dict(
                schema="protodd-active-features-v1", registry_sha256=registry_hash,
                build_options=active_options, build_inputs=active_inputs,
                runtime_controls=runtime_contract))
        (root / "Protodd.build-manifest.json").write_text(json.dumps(build_manifest))
        return dict(template=template, output=root / "run", dll=dll, opponents=["Enemy"], maps=["maps/fixture.scx"])

    def test_training_requires_registered_build_and_final_test_requires_tournament_values(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            sidecar = args["dll"].parent / "Protodd.build-manifest.json"
            sidecar.unlink()
            with self.assertRaisesRegex(ValueError, "feature-bound build manifest"):
                prepare(**args, purpose="training")
            self.assertFalse(args["output"].exists())

            args = self.fixture(Path(temp) / 'incomplete-runtime')
            sidecar = args["dll"].parent / "Protodd.build-manifest.json"
            build_manifest = json.loads(sidecar.read_text())
            build_manifest["registered_feature_manifest"]["runtime_controls"] = []
            sidecar.write_text(json.dumps(build_manifest))
            with self.assertRaisesRegex(ValueError, "incomplete runtime feature coverage"):
                prepare(**args, purpose="training")
            self.assertFalse(args["output"].exists())

            args = self.fixture(Path(temp) / "release")
            prepare(**args, purpose="final-test")
            self.assertTrue(verify(args["output"])["verified"])

    def test_final_test_rejects_non_tournament_compile_features(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp), {"PROTODD_NATIVE_ALLIN_OPENING": "ON"})
            with self.assertRaisesRegex(ValueError, "non-tournament feature enabled"):
                prepare(**args, purpose="final-test")
            self.assertFalse(args["output"].exists())

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

    def test_manager_html_results_are_mutable_outputs(self):
        with tempfile.TemporaryDirectory() as temp:
            args = self.fixture(Path(temp))
            dashboard = args["template"] / "server/html/results"
            dashboard.mkdir(parents=True)
            (dashboard / "detailed_results.txt").write_text("old")
            manifest = prepare(**args)
            self.assertNotIn("server/html/results/detailed_results.txt", manifest["components"])
            (args["output"] / "server/html/results/detailed_results.txt").write_text("new")
            self.assertTrue(verify(args["output"])["verified"])

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
