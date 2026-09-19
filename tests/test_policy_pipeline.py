"""Synthetic end-to-end regression; never deploy its deliberately fake policy."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

trainer = Path(sys.argv[1]).resolve()
script = Path(__file__).resolve().parents[1]/'tools/train_policy.py'
with tempfile.TemporaryDirectory(prefix='protodd-policy-test-') as temp:
    root = Path(temp)
    run = root/'run'
    server = run/'server'
    archive = server/'replays/bot-write/game-0/TerranTodd/host-fixture'
    archive.mkdir(parents=True)
    (run/'manifest.json').write_text(json.dumps(dict(bot='TerranTodd', race='Terran')))
    (server/'server_settings.json').write_text(json.dumps(dict(tournamentModuleSettings=dict(
        timeoutLimits=[dict(timeInMS=55, frameCount=320)]))))
    common = dict(gameID=0, map='fixture', gameEndType='NORMAL', crash=False,
                  gameTimeout=False, finalFrame=1000, timers=[dict(timeInMS=55, frameCount=0)])
    rows = [dict(common, reportingBot='TerranTodd', opponentBot='Opponent', won=True),
            dict(common, reportingBot='Opponent', opponentBot='TerranTodd', won=False)]
    results = server/'results.jsonl'
    results.write_text('\n'.join(json.dumps(r) for r in rows))
    (archive/'PolicyTrace.log').write_text('BEGIN,1,Terran,Protoss,Terran_Protoss_v1,123,train\n'
        'DECISION,0,1,0,15\nDECISION,480,2,1,15\nEND,1000,1\n')
    command = [sys.executable, str(script), str(run), '--game-ids', '0', '--trainer', str(trainer)]
    subprocess.run(command + ['--output', str(root/'valid')], check=True)
    assert (root/'valid/Policy.q').stat().st_size > 0
    manifest = json.loads((root/'valid/manifest.json').read_text())
    assert manifest['transitionCount'] == 2
    assert manifest['reviewedEpisodes'][0]['reward'] == 1
    subprocess.run(command + ['--previous-policy', str(root/'valid'), '--output', str(root/'merged')], check=True)
    assert (root/'merged/Policy.q').read_bytes() == (root/'valid/Policy.q').read_bytes()
    rows[1]['timers'] = [dict(timeInMS=55, frameCount=320)]
    results.write_text('\n'.join(json.dumps(r) for r in rows))
    rejected = subprocess.run(command + ['--output', str(root/'invalid')], capture_output=True)
    assert rejected.returncode != 0
    assert not (root/'invalid').exists()
print('Validated policy pipeline passes; runtime-limit win rejected')
