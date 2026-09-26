"""Audit frozen shadow games. No result in this report authorizes model control."""
import argparse
import json
from pathlib import Path

import numpy as np
import torch

from .arena import verify
from .production_demand_fit import DemandModel
from .schema import sha256


def feedback_audit(text):
    tickets={};life={};issues=[];summary=None;misses=0
    names=('Protoss_Probe','Protoss_Pylon','Protoss_Gateway','Protoss_Assimilator',
        'Protoss_Cybernetics_Core','Protoss_Zealot','Protoss_Dragoon','Protoss_Nexus')
    lines=[line.split(',') for line in text.splitlines()]
    for f in lines:
        if f[0]=='LIFECYCLE' and len(f)>5 and f[3]=='self':
            life.setdefault((int(f[4]),f[2],f[5]),[]).append(int(f[1]))
    for f in lines:
        kv=dict(x.split('=',1) for x in f[2:] if '=' in x)
        if f[0]=='PRODUCTION_RESERVED':
            key=int(kv['ticket'])
            if key in tickets:issues.append('duplicate ticket reservation')
            tickets[key]=dict(action=int(kv['action']),reserved=int(f[1]),spent=None,completed=None,product=None,accepted=False)
        elif f[0] in ('PRODUCTION_DISPATCH','PRODUCTION_SPENT','PRODUCTION_COMPLETED','PRODUCTION_FAILED'):
            key=int(kv['ticket']);t=tickets.get(key)
            if t is None:issues.append('feedback without reservation');continue
            frame=int(f[1])
            if f[0]=='PRODUCTION_DISPATCH':t['accepted']=kv['api_accepted']=='1'
            else:
                product=int(kv['product'])
                if not t['accepted'] or frame<t['reserved']:issues.append('feedback without accepted chronological dispatch')
                if f[0]=='PRODUCTION_SPENT':
                    if t['spent'] is not None:issues.append('duplicate spend release')
                    t['spent']=frame;t['product']=product
                    observed=life.get((product,'create',names[t['action']]),[])
                    if t['action']==3:observed+=life.get((product,'morph',names[t['action']]),[])
                    if not any(t['reserved']<=n<=frame for n in observed):issues.append('spend without matching observed product creation')
                else:
                    if t['spent'] is None or product!=t['product']:issues.append('outcome before bound product/spending')
                    if f[0]=='PRODUCTION_COMPLETED':
                        if t['completed'] is not None:issues.append('duplicate completion')
                        t['completed']=frame
                        observed=life.get((product,'complete',names[t['action']]),[])
                        if not any(t['reserved']<=n<=frame for n in observed):issues.append('completion without lifecycle evidence')
        elif f[0]=='PRODUCTION_RESERVE_MISS':misses+=1
        elif f[0]=='PRODUCTION_FEEDBACK_SUMMARY':summary=dict(x.split('=',1) for x in f[1:])
    return dict(tickets=len(tickets),spent=sum(t['spent'] is not None for t in tickets.values()),
        completed=sum(t['completed'] is not None for t in tickets.values()),reserve_misses=misses,
        issues=issues,summary=summary,
        gate_pass=bool(tickets) and summary is not None and not issues and misses==0 and
            sum(t['completed'] is not None for t in tickets.values())>=5)


def audit(directory, model):
    log=directory/'Protodd.log'
    events=[];issues=[];disabled=[];shadow=0
    text=log.read_text(errors='replace')
    for line in text.splitlines():
        fields=line.split(',');kv=dict(x.split('=',1) for x in fields[2:] if '=' in x)
        if fields[0]=='PRODUCTION_ACCEPT':
            events.append((int(fields[1]),int(kv['action']),int(kv['delay'])))
        elif fields[0]=='PRODUCTION_ISSUE':issues.append(dict(frame=int(fields[1]),**kv))
        elif fields[0]=='PRODUCTION' and 'status=disabled' in fields:disabled.append(line)
        elif fields[0]=='PRODUCTION_SHADOW':shadow+=1
    data=(directory/'production-inputs.bin').read_bytes()
    dtype=np.dtype([('frame','<i4'),('features','<f4',(614,)),('counts','<i4',(8,))])
    if data[:8]!=b'PTDLIVE1' or (len(data)-8)%dtype.itemsize:raise ValueError('incomplete input audit')
    rows=np.frombuffer(data[8:],dtype=dtype)
    if not len(rows) or (np.diff(rows['frame'])<=0).any():raise ValueError('empty/backwards samples')
    if shadow!=len(rows):raise ValueError('prediction log and tensor count mismatch')
    expected=np.zeros((len(rows),16),np.float32);expected[:,8:]=1
    for i,frame in enumerate(rows['frame']):
        for action in range(8):
            prior=[t for t,a,_ in events if a==action and t<frame]
            expected[i,action]=min(4,sum(t>=frame-240 for t in prior))/4
            if prior:expected[i,action+8]=min(2400,int(frame)-prior[-1])/2400
    history_error=float(np.max(np.abs(expected-rows['features'][:,-16:])))
    with torch.no_grad():
        result=model(torch.tensor(rows['features'].copy())).numpy()
    quantities=(result>=0).sum(-1)
    mismatch=int(np.any(quantities!=rows['counts'],axis=1).sum())
    timing_path=directory/'production-callback-us.bin'
    timing=np.fromfile(timing_path,dtype='<i8') if timing_path.exists() else np.array([])
    timing_report=dict(samples=len(timing),p95_us=float(np.percentile(timing,95)) if len(timing) else None,
        p99_us=float(np.percentile(timing,99)) if len(timing) else None,
        max_us=int(timing.max()) if len(timing) else None,
        over_55ms=int((timing>55000).sum()),over_1s=int((timing>1000000).sum()),over_10s=int((timing>10000000).sum()))
    return dict(directory=str(directory),hashes={p.name:sha256(p) for p in (log,directory/'production-inputs.bin',timing_path) if p.exists()},
        rows=len(rows),last_observation=int(rows['frame'][-1]),confirmed_events=len(events),
        events_by_action={str(a):sum(e[1]==a for e in events) for a in range(8)},
        accepted_api_requests=sum(i['api_accepted']=='1' for i in issues),
        max_confirmation_delay=max((e[2] for e in events),default=0),
        disabled=disabled,history_max_error=history_error,model_mismatched_rows=mismatch,
        callback=timing_report,
        feedback=feedback_audit(text),
        early_window_covered=int(rows['frame'][-1])>=7176 and not disabled,
        history_and_model_parity=history_error<1e-6 and mismatch==0,
        callback_under_limits=len(timing)>0 and timing_report['over_55ms']<320 and
            timing_report['over_1s']<10 and timing_report['over_10s']==0)


def review(run,fit):
    verify(run);torch.set_num_threads(2)
    spec=json.loads((fit/'run.json').read_text())
    saved=torch.load(fit/'resume.pt',map_location='cpu',weights_only=True)['model']
    model=DemandModel(saved['mean'],saved['scale'],spec['width']);model.load_state_dict(saved);model.eval()
    # Bind the checkpoint to the exact confirmed/exported model used in the arena.
    native=Path('artifacts/replay-learning/production-demand-native-20260924')
    native_spec=json.loads((native/'run.json').read_text())
    if sha256(fit/'resume.pt')!=native_spec['checkpoint_sha256']:raise ValueError('checkpoint differs')
    if sha256(run/'server/bots/Protodd/read/ProductionDemand.bin')!=sha256(native/'ProductionDemand.bin'):
        raise ValueError('campaign model differs')
    directories=sorted((run/'server/replays/bot-write').glob('game-*/Protodd/received'))
    games=[audit(d,model) for d in directories]
    manifest=json.loads((run/'manifest.json').read_text())
    reports=[json.loads(line) for line in (run/'server/results.jsonl').read_text().splitlines() if line.strip()]
    pairs={};schedule={r['gameID']:r for r in map(json.loads,(run/'server/games.jsonl').read_text().splitlines())}
    for row in reports:pairs.setdefault(row['gameID'],[]).append(row)
    report_pairs_complete=set(pairs)==set(schedule) and all(len(pair)==2 and
        {r['reportingBot'] for r in pair}=={schedule[gid]['homeBot'],schedule[gid]['awayBot']} and
        all(r['opponentBot']==pair[1-i]['reportingBot'] and r['finalFrame']==pair[0]['finalFrame'] and
            r['map']==schedule[gid]['map'] and r['gameEndType']=='NORMAL' and not r['crash'] and not r['gameTimeout']
            for i,r in enumerate(pair)) for gid,pair in pairs.items())
    return dict(schema='protodd-production-live-review-v1',campaign=str(run.resolve()),
        mode=(run/'server/bots/Protodd/read/ProductionDemand-mode.txt').read_text().strip(),
        manifest_sha256=sha256(run/'manifest.json'),source_sha256=sha256(__file__),
        games=games,report_pairs_complete=report_pairs_complete,
        shadow_gate_pass=len(games)==manifest['games'] and report_pairs_complete and all(
            g['early_window_covered'] and g['history_and_model_parity'] and g['callback_under_limits'] for g in games),
        feedback_gate_pass=len(games)==manifest['games'] and report_pairs_complete and all(g['feedback']['gate_pass'] for g in games),
        notes=['Bounded shadow games are not strength evidence.',
            'Input audit checks history causality and model parity; base observation parity has separate tests.',
            'Callback timer includes budget/logging; only the final timing append is outside.',
            'Feedback is checked against real product lifecycle events; the recorded mode distinguishes shadow from bounded local control.'],
        live_control_allowed=False,promotion_eligible=False)

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('run',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--fit',type=Path,default=Path('artifacts/replay-learning/production-demand-development-20260924'))
    a=p.parse_args()
    if a.output.exists():raise FileExistsError(a.output)
    r=review(a.run,a.fit);a.output.write_text(json.dumps(r,indent=2)+'\n')
    print(json.dumps(r,indent=2))
