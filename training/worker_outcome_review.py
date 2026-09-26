"""Adjudicate matched worker-allocation training interventions from actual games."""
import argparse
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256

PROFILES = ('baseline', 'plus-one', 'plus-two')
OBSERVATIONS = (2400, 3600, 4800, 6000, 7200)


def read_game(log, expected_profile):
    match = None
    declared = []
    interventions = []
    states = {}
    enemy_activity = False
    errors = []
    forbidden_control = []
    for line in log.read_text(errors='replace').splitlines():
        parts = line.split(',')
        kind = parts[0]
        values = dict(item.split('=', 1) for item in parts[1:] if '=' in item)
        if kind == 'MATCH':
            match = values
        elif kind == 'WORKER_TRAINING_MODE':
            declared.append(dict(profile=parts[1], enabled=values.get('enabled')))
        elif kind == 'WORKER_TRAINING':
            interventions.append(dict(frame=int(parts[1]),
                                      committed=int(values['committed']),
                                      before=int(values['before']), goal=int(values['goal']),
                                      before_priority=int(values['beforePriority']),
                                      priority=int(values['priority'])))
        elif kind == 'STATE':
            frame = int(parts[1])
            if frame in OBSERVATIONS:
                states[frame] = dict(probes=int(values['probes']), army=int(values['army']),
                                     minerals=int(parts[7]), supply_used=int(parts[9]),
                                     enemy_visible_army=int(values.get('enemyVisibleArmy', 0)))
            enemy_activity |= int(values.get('enemyVisibleArmy', 0)) > 0
        elif kind == 'ERROR':
            errors.append(line)
        elif kind in ('PRODUCTION_CONTROL', 'WHOLE_GAME_CONTROL'):
            forbidden_control.append(line)
    if (match is None or declared != [dict(profile=expected_profile, enabled='1')]
            or any(x['frame'] < 2400 or x['frame'] >= 7200 or x['goal'] > 32 or
                   (x['goal'] <= x['before'] and x['priority'] <= x['before_priority'])
                   for x in interventions)):
        raise ValueError('intervention identity or bounds differ from frozen campaign')
    if expected_profile == 'baseline' and interventions:
        raise ValueError('baseline was modified')
    # Emergency conditions can correctly suppress the treatment for a game.
    # Exposure is reported, never invented from a requested mode.
    return dict(match=match, declared=declared[0], interventions=len(interventions),
                exposed=bool(interventions), states=states, enemy_activity=enemy_activity,
                errors=errors, forbidden_control=forbidden_control,
                log_sha256=sha256(log), log=str(log.resolve()))


def campaign(run, expected_profile, games):
    verify(run)
    inspection = inspect(run)
    schedule = {g['gameID']: g for g in map(json.loads, (run/'server/games.jsonl').read_text().splitlines())}
    manifest = json.loads((run/'manifest.json').read_text())
    read = run/'server/bots/Protodd/read'
    if (manifest['purpose'] != 'training' or manifest['games'] != games or
            (read/'WorkerTraining-mode.txt').read_text().strip() != expected_profile or
            (read/'Protodd-learning-mode.txt').read_text().strip() != 'frozen' or
            (read/'Policy-mode.txt').read_text().strip() != 'frozen' or
            (read/'ProductionDemand-mode.txt').read_text().strip() != 'off'):
        raise ValueError('campaign training scope changed')
    rows = []
    for game in inspection['structurally_valid']:
        gid = game['game_id']
        log = run/f'server/replays/bot-write/game-{gid}/Protodd/received/Protodd.log'
        evidence = read_game(log, expected_profile)
        rows.append(dict(**game, **evidence,
                         host=schedule[gid]['homeBot'] == 'Protodd',
                         map=schedule[gid]['map']))
    return dict(campaign=str(run.resolve()), manifest_sha256=sha256(run/'manifest.json'),
                components=manifest['components'], games=rows,
                scheduled=inspection['scheduled'], excluded=inspection['excluded'])


def review(plan_path, paths):
    plan = json.loads(plan_path.read_text())
    expected = plan['games_per_condition']
    runs = {profile: campaign(Path(paths[profile]), profile, expected) for profile in PROFILES}
    games = {p: {g['game_id']: g for g in runs[p]['games']} for p in PROFILES}
    all_games = [g for rows in games.values() for g in rows.values()]
    healthy = all(runs[p]['scheduled'] == expected and len(games[p]) == expected and
                  not runs[p]['excluded'] for p in PROFILES)
    matched = healthy and all(
        all(games[p][gid]['match'] == games['baseline'][gid]['match'] and
            games[p][gid]['host'] == games['baseline'][gid]['host'] and
            games[p][gid]['map'] == games['baseline'][gid]['map'] and
            int(games[p][gid]['match']['seed']) == plan['seed_base']+gid
            for p in PROFILES) for gid in range(expected))
    same_inputs = True
    fixed = ('server/bots/Protodd/AI/Protodd.dll',
             'server/bots/UABTerran/AI/UABTerran.dll',
             'server/required/maps.zip', 'client1/client.jar', 'client2/client.jar')
    for component in fixed:
        same_inputs &= len({runs[p]['components'][component] for p in PROFILES}) == 1
    runtime = healthy and all(g['enemy_activity'] and not g['errors'] and
                              not g['forbidden_control'] for g in all_games)
    if not (matched and same_inputs and runtime):
        return dict(schema='protodd-worker-outcome-pilot-v1', plan_sha256=sha256(plan_path),
                    source_sha256=sha256(__file__), campaigns=runs,
                    checks=dict(healthy=healthy, matched=matched,
                                identical_inputs=same_inputs, runtime=runtime),
                    usable_training_outcomes=False, outcomes=[],
                    promotion_eligible=False, final_test_opened=False)
    # Samples remain descriptive diagnostics. Wins are the actual outcome labels.
    outcomes = [dict(profile=p, game_id=gid, seed=int(g['match']['seed']),
                     map=g['map'], host=g['host'], won=g['won'], frame=g['frame'],
                     exposed=g['exposed'], interventions=g['interventions'],
                     observations=g['states'], log_sha256=g['log_sha256'])
                for p in PROFILES for gid, g in sorted(games[p].items())]
    def mean(profile, frame, key):
        found = [g['states'][frame][key] for g in games[profile].values() if frame in g['states']]
        return sum(found)/len(found) if len(found) == expected else None
    summary = {}
    ref_wins = sum(g['won'] for g in games['baseline'].values())
    ref_army = mean('baseline', 7200, 'army')
    ref_probes = mean('baseline', 6000, 'probes')
    for p in PROFILES:
        wins = sum(g['won'] for g in games[p].values())
        army = mean(p, 7200, 'army')
        probes = mean(p, 6000, 'probes')
        summary[p] = dict(wins=wins, mean_army_7200=army,
                          mean_probes_6000=probes,
                          exposed_games=sum(g['exposed'] for g in games[p].values()),
                          screen_more_training=(p != 'baseline' and wins > ref_wins and
                              army is not None and ref_army is not None and army >= .8*ref_army and
                              probes is not None and ref_probes is not None and probes >= ref_probes+1 and
                              all(g['exposed'] for g in games[p].values())))
    return dict(schema='protodd-worker-outcome-pilot-v1', plan_sha256=sha256(plan_path),
                source_sha256=sha256(__file__), campaigns=runs, summary=summary,
                checks=dict(healthy=healthy, matched=matched,
                            identical_inputs=same_inputs, runtime=runtime),
                usable_training_outcomes=True, outcomes=outcomes,
                promotion_eligible=False, strength_proven=False, final_test_opened=False,
                note='Twelve matched training games provide intervention outcomes, not enough evidence for promotion or a general learned policy.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('baseline', type=Path)
    parser.add_argument('plus_one', type=Path)
    parser.add_argument('plus_two', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, dict(baseline=args.baseline,
                                   **{'plus-one': args.plus_one, 'plus-two': args.plus_two}))
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k: result.get(k) for k in ('checks', 'summary', 'usable_training_outcomes')}))
