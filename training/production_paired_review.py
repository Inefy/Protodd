"""Seed-matched full-game pilot. A pass permits further strength evaluation only."""
import argparse
import json
from pathlib import Path

from .arena import inspect,verify
from .production_live_review import review as live_review
from .production_control_screen import measurements
from .schema import sha256


def condition(run,fit):
    verify(run);result=inspect(run);audit=live_review(run,fit)
    schedule={r['gameID']:r for r in map(json.loads,(run/'server/games.jsonl').read_text().splitlines())}
    rows=[]
    for game in result['structurally_valid']:
        path=run/f"server/replays/bot-write/game-{game['game_id']}/Protodd/received/Protodd.log"
        match=None;activity=False;errors=[];controlled=0
        for line in path.read_text(errors='replace').splitlines():
            f=line.split(',')
            if f[0]=='MATCH':match=dict(x.split('=',1) for x in f[1:] if '=' in x)
            elif f[0]=='STATE':
                data=dict(x.split('=',1) for x in f[14:] if '=' in x)
                activity|=int(data.get('enemyVisibleArmy',0))>0
            elif f[0]=='ERROR' or f[0]=='PRODUCTION' and 'status=disabled' in f:errors.append(line)
            elif f[0]=='PRODUCTION_CONTROL' and 'accepted=1' in f:controlled+=1
        rows.append(dict(**game,match=match,enemy_activity=activity,errors=errors,controlled=controlled,
            was_host=schedule[game['game_id']]['homeBot']=='Protodd',log_sha256=sha256(path)))
    return dict(games=rows,excluded=result['excluded'],audit=audit,manifest_sha256=sha256(run/'manifest.json'))


def review(candidate,reference,fit):
    c=condition(candidate,fit);r=condition(reference,fit)
    by_id=lambda data:{g['game_id']:g for g in data['games']}
    cg,rg=by_id(c),by_id(r)
    healthy=len(cg)==len(rg)==2 and not c['excluded'] and not r['excluded']
    paired=healthy and all(cg[i]['match']==rg[i]['match'] and cg[i]['was_host']==rg[i]['was_host'] and
        int(cg[i]['match']['seed'])==20260924+i for i in cg)
    runtime=healthy and all(not g['errors'] and g['enemy_activity'] for data in (c,r) for g in data['games']) and all(
        g['history_and_model_parity'] and g['callback_under_limits'] and not g['disabled'] and not g['feedback']['issues']
        for data in (c,r) for g in data['audit']['games'])
    wins_c=sum(g['won'] for g in cg.values());wins_r=sum(g['won'] for g in rg.values())
    manifests=[json.loads((run/'manifest.json').read_text())['components'] for run in (candidate,reference)]
    fixed=['server/bots/Protodd/AI/Protodd.dll','server/bots/Protodd/read/ProductionDemand.bin',
        'server/bots/UABTerran/AI/UABTerran.dll','server/required/maps.zip','client1/client.jar','client2/client.jar']
    bound=all(manifests[0][key]==manifests[1][key] for key in fixed)
    checks=dict(healthy_games=healthy,matched_seeds_and_sides=paired,identical_inputs_except_mode=bound,
        runtime=runtime,actual_candidate_control=healthy and all(g['controlled']>0 for g in cg.values()),
        reference_uncontrolled=all(g['controlled']==0 for g in rg.values()),win_improvement=wins_c>wins_r)
    return dict(schema='protodd-production-paired-pilot-v1',source_sha256=sha256(__file__),candidate=c,reference=r,
        wins=dict(candidate=wins_c,reference=wins_r),checks=checks,advance_to_72=all(checks.values()),
        statistically_established_strength=False,tournament_control_allowed=False,
        note='Two pairs are a pilot screen, not a statistically established win-rate estimate.')

if __name__=='__main__':
    p=argparse.ArgumentParser();p.add_argument('candidate',type=Path);p.add_argument('reference',type=Path);p.add_argument('output',type=Path)
    p.add_argument('--fit',type=Path,default=Path('artifacts/replay-learning/production-demand-development-20260924'))
    a=p.parse_args()
    if a.output.exists():raise FileExistsError(a.output)
    report=review(a.candidate,a.reference,a.fit);a.output.write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps({k:report[k] for k in ('wins','checks','advance_to_72')},indent=2))
