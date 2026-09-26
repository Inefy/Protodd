"""Fixed early-production screen; these unmatched-seed scenarios are not win-rate evidence."""
import argparse
import json
from pathlib import Path

from .arena import verify
from .production_live_review import review
from .schema import sha256


def measurements(run):
    rows=[]
    for log in sorted((run/'server/replays/bot-write').glob('game-*/Protodd/received/Protodd.log')):
        probes=None;army=0;controlled=0;violations=[];seed=None
        for line in log.read_text(errors='replace').splitlines():
            f=line.split(',');kv=dict(x.split('=',1) for x in f[2:] if '=' in x)
            if f[0]=='MATCH':seed=dict(x.split('=',1) for x in f[1:] if '=' in x).get('seed')
            if f[0]=='STATE' and int(f[1])==3600:probes=int(kv['probes'])
            if f[0]=='PRODUCTION_ACCEPT' and int(f[1])<7200 and int(kv['action']) in (5,6):army+=1
            if f[0]=='PRODUCTION_CONTROL' and kv.get('accepted')=='1':controlled+=1
            if f[0]=='ERROR' or f[0]=='PRODUCTION' and 'status=disabled' in f:violations.append(line)
        if probes is None:raise ValueError('missing prespecified frame-3600 observation')
        rows.append(dict(log=str(log),sha256=sha256(log),seed=seed,probes_3600=probes,army_starts_7200=army,
            controlled_commands=controlled,errors=violations))
    return rows


def screen(candidate,reference,fit):
    verify(reference);report=review(candidate,fit)
    c=measurements(candidate);r=measurements(reference)
    if len(c)!=2 or len(r)!=2:raise ValueError('screen requires two games per condition')
    mean=lambda data,key:sum(g[key] for g in data)/len(data)
    cp,rp=mean(c,'probes_3600'),mean(r,'probes_3600')
    ca,ra=mean(c,'army_starts_7200'),mean(r,'army_starts_7200')
    checks=dict(live_inputs_and_timing=report['shadow_gate_pass'],feedback=report['feedback_gate_pass'],
        actual_control=all(g['controlled_commands']>0 for g in c),no_fallback_or_error=not any(g['errors'] for g in c),
        probes=cp>=.9*rp,army=ca>=.8*ra)
    return dict(schema='protodd-production-control-screen-v1',source_sha256=sha256(__file__),
        candidate_manifest_sha256=sha256(candidate/'manifest.json'),reference_manifest_sha256=sha256(reference/'manifest.json'),
        candidate=c,reference=r,live_audit=report,
        mean_probes=dict(candidate=cp,reference=rp,minimum_ratio=.9),
        mean_army_starts=dict(candidate=ca,reference=ra,minimum_ratio=.8),
        checks=checks,passed=all(checks.values()),seed_matched=False,strength_proven=False,
        tournament_control_allowed=False,final_test_opened=False)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('reference',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--fit',type=Path,default=Path('artifacts/replay-learning/production-demand-development-20260924'))
    a=p.parse_args()
    if a.output.exists():raise FileExistsError(a.output)
    r=screen(a.candidate,a.reference,a.fit);a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps({k:r[k] for k in ('passed','checks','mean_probes','mean_army_starts')},indent=2))
