"""Review frozen audit fixtures, full callback timing and repeated-control games."""
import argparse,json,struct,statistics,random
from pathlib import Path
from .schema import sha256
from .pvz_approach_generalization_review import campaign

def timing(path):
    raw=path.read_bytes()
    if not raw or len(raw)%8:raise ValueError(f'Invalid timing file: {path}')
    values=sorted(v[0] for v in struct.iter_unpack('<q',raw))
    if values[0]<0:raise ValueError('Negative callback time')
    return dict(samples=len(values),p99_us=values[int(.99*(len(values)-1))],max_us=values[-1],
        at_least_42000=sum(v>=42000 for v in values),at_least_55000=sum(v>=55000 for v in values),sha256=sha256(path))

def fixtures(root,arm):
    result={}
    for case in ('storm-allies','storm-clear','producer','prerequisite','combat'):
        path=root/f'scenarios-verified-{arm}'/f'{case}.csv'
        lines=[line.split(',') for line in path.read_text().splitlines()]
        checks={line[1]:line[2]=='1' for line in lines if line[0]=='CHECK'}
        if not any(line[0]=='DONE' for line in lines):raise ValueError(f'Incomplete fixture {path}')
        if not all(v for k,v in checks.items() if k.startswith('fixture-')):raise ValueError(f'Fixture invalid: {path}')
        result[case]=dict(checks=checks,passed=all(checks.values()),sha256=sha256(path),
            map_sha256=sha256(path.with_suffix('.scx')))
    return result

def review(plan_path):
    root=plan_path.parent;plan=json.loads(plan_path.read_text());gates=plan['gates']
    for name,digest in plan['sha256'].items():
        if sha256(root/name)!=digest:raise ValueError(f'Frozen input changed: {name}')
    for name,digest in plan['repo_sha256'].items():
        if sha256(Path(name))!=digest:raise ValueError(f'Reviewer/owner changed: {name}')
    engines={arm:fixtures(root,arm) for arm in ('reference','candidate')}
    engine_pass=all(v['passed'] for v in engines['candidate'].values())
    counterfactual=all(not engines['reference'][case]['passed'] for case in ('storm-allies','producer','prerequisite','combat')) and engines['reference']['storm-clear']['passed']
    for case in engines['candidate']:
        if engines['candidate'][case]['map_sha256']!=engines['reference'][case]['map_sha256']:raise ValueError('Fixture map mismatch')
    stress={arm:timing(root/f'load-{arm}/full-callback-us.bin') for arm in ('candidate','reference')}
    arms={arm:campaign(root/arm,plan['games_per_arm'],plan['frame_limit']) for arm in ('reference-a','reference-b','candidate')}
    a,b,c=arms.values()
    checks=dict(engine=engine_pass,counterfactual=counterfactual,
        common_inputs=a['common_inputs_sha256']==b['common_inputs_sha256']==c['common_inputs_sha256'],
        dlls=a['dll_sha256']==b['dll_sha256']==plan['reference_dll_sha256'] and c['dll_sha256']==plan['candidate_dll_sha256'],
        paired=all(x[key]==y[key]==z[key] for x,y,z in zip(a['rows'],b['rows'],c['rows']) for key in ('seed','map_hash','map','home')),
        seeds=all(r['seed']==plan['seed_base']+r['game_id'] for r in a['rows']))
    schedules={arm:{g['gameID']:g for g in map(json.loads,(root/arm/'server/games.jsonl').read_text().splitlines())} for arm in arms}
    for arm,data in arms.items():
        for row in data['rows']:
            game=schedules[arm][row['game_id']]
            row['opponent']=next(name for name in (game['homeBot'],game['awayBot']) if name!='Protodd')
            path=root/arm/f"server/replays/bot-write/game-{row['game_id']}/Protodd/received/production-callback-us.bin"
            row['callback']=timing(path)
            if abs(row['callback']['samples']-(row['frame']+1))>2:raise ValueError(f'Incomplete callback coverage {path}')
    all_timing=list(stress.values())+[r['callback'] for arm in arms.values() for r in arm['rows']]
    checks['callback_budget']=all(t['p99_us']<gates['callback_p99_us'] and t['max_us']<gates['callback_max_us'] for t in all_timing)
    def summary(rows):
        return dict(games=len(rows),wins=sum(r['decisive_won'] is True for r in rows),capped=sum(r['capped'] for r in rows),
            army8400=statistics.mean(r['states'].get('8400',{}).get('army',0) for r in rows),
            probes8400=statistics.mean(r['states'].get('8400',{}).get('probes',0) for r in rows),
            early_losses=statistics.mean(r['early_losses'] for r in rows),
            first_defender=statistics.mean(r['first_defender'] if r['first_defender'] is not None else r['frame'] for r in rows),
            mean_frames=statistics.mean(r['frame'] for r in rows),
            callback_max_us=max(r['callback']['max_us'] for r in rows))
    summaries={arm:summary(data['rows']) for arm,data in arms.items()}
    by_opponent={op:{arm:summary([r for r in data['rows'] if r['opponent']==op]) for arm,data in arms.items()} for op in plan['opponents']}
    metrics={}
    for op,group in {'all':summaries,**by_opponent}.items():
        x,y,z=(group[arm] for arm in arms)
        metrics[op]=dict(outcomes=z['wins']>=max(x['wins'],y['wins'])-gates['max_matchup_win_drop'],
            army=z['army8400']>=min(x['army8400'],y['army8400'])-gates['max_mean_army_drop'],
            workers=z['probes8400']>=min(x['probes8400'],y['probes8400'])-gates['max_mean_worker_drop'],
            early_losses=z['early_losses']<=max(x['early_losses'],y['early_losses'])+gates['max_mean_early_loss_increase'],
            defender=z['first_defender']<=max(x['first_defender'],y['first_defender'])+gates['max_mean_defender_delay'])
    checks['gameplay_regression_screen']=all(v for group in metrics.values() for v in group.values())
    x,y,z=(summaries[arm] for arm in arms)
    checks['development_win_gain']=z['wins']>=max(x['wins'],y['wins'])+gates['minimum_win_gain']
    checks['decisive_games']=all(s['capped']==0 for s in summaries.values())
    effects=[];repeat=[]
    for p,q,r in zip(a['rows'],b['rows'],c['rows']):
        if all(t['decisive_won'] is not None for t in (p,q,r)):
            effects.append(int(r['decisive_won'])-(int(p['decisive_won'])+int(q['decisive_won']))/2)
            repeat.append(int(q['decisive_won'])-int(p['decisive_won']))
    rng=random.Random(20260926)
    def interval(values):
        if not values:return None
        means=sorted(statistics.mean(rng.choices(values,k=len(values))) for _ in range(10000))
        return [means[249],means[9749]]
    return dict(schema='protodd-audit-validation-v1',plan_sha256=sha256(plan_path),checks=checks,
        engine=engines,stress_timing=stress,arms=arms,summaries=summaries,per_opponent=by_opponent,
        gameplay_checks=metrics,paired_win_effect=statistics.mean(effects) if effects else None,
        paired_win_bootstrap_95=interval(effects),same_dll_win_bootstrap_95=interval(repeat),
        advance_to_strength_gate=all(checks.values()),strength_validated=False,promotion_allowed=False,
        interval_note='Descriptive seed-cluster bootstrap; both reference repeats stay in each cluster. Capped triples excluded only from decisive win estimates.',
        state_note='Early-ended games without frame 8400 count zero army/workers, never silently disappear.')

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('plan',type=Path);parser.add_argument('output',type=Path);args=parser.parse_args()
    if args.output.exists():raise FileExistsError(args.output)
    result=review(args.plan);args.output.write_text(json.dumps(result,indent=2)+'\n')
    print(json.dumps({k:result[k] for k in ('checks','summaries','advance_to_strength_gate')}))
