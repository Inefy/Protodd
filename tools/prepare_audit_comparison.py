"""Prepare (never launch) source-pinned repeated-control audit games."""
from pathlib import Path
import json,shutil,sys
ROOT=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT))
from training.arena import prepare, verify
from training.schema import sha256
OUT=ROOT/'build/audit-validation-20260926'
if __name__=='__main__':
    for arm in ('reference-a','reference-b','candidate'):
        run=OUT/arm
        manifest=prepare(ROOT/'ladder/runs/ladder5-aiide2025-20260906/tournament',run,
            OUT/('Protodd.candidate.dll' if arm=='candidate' else 'Protodd.reference.dll'),
            ['McRaveZ','Steamhammer','BananaBrain'],
            ['maps/aiide/(2)Benzene.scx','maps/aiide/(2)Destination.scx'],
            purpose='development',rounds=2,port=1358,frame_limit=30000,
            server_jar=ROOT/'build/pvz-available-composition-20260926/reference-a/server/server.jar',
            client_bundle=OUT/'client-bundle')
        marker=run/'server/bots/Protodd/read/CallbackAudit-mode.txt';marker.write_text('on\n')
        manifest['components'][marker.relative_to(run).as_posix()]=sha256(marker)
        settings=run/'server/server_settings.json';s=json.loads(settings.read_text())
        for bot in s['bots']:
            if bot['BotName']=='Steamhammer':bot['Race']='Terran'
        settings.write_text(json.dumps(s,indent=2)+'\n')
        manifest['components']['server/server_settings.json']=sha256(settings)
        (run/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
        print(arm,verify(run),flush=True)
