"""Preserve baseline event timelines for human loss review, never training labels.

Only consistent NORMAL report pairs receive timelines. Observed opponent units
and combat deaths support activity review; they do not prove optimal play.
"""
import argparse
from collections import Counter
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256
from tools.log_analyzer import parse_key_values


STATE_FIELDS = ('probes', 'army', 'enemyVisibleArmy', 'gateways', 'nexuses',
                'core', 'dragoons', 'zealots', 'gasWorkers', 'gasTarget',
                'minedMinerals', 'minedGas', 'bases', 'desiredWorkers', 'busy',
                'workersStatus', 'actions')


def timeline(path):
    states, life, losses, health, match = [], [], [], [], None
    with path.open(encoding='utf-8', errors='replace') as stream:
        for number, raw in enumerate(stream, 1):
            f = raw.rstrip().split(',')
            if f[0] == 'MATCH':
                match = parse_key_values(f[1:])
            elif f[0] == 'STATE' and len(f) >= 14:
                extra = parse_key_values(f[14:])
                states.append(dict(frame=int(f[1]), line=number, strategy=f[2],
                    posture=f[3], enemy_plan=f[4], minerals=int(f[7]), gas=int(f[8]),
                    supply=int(f[9]), supply_total=int(f[10]), macro_status=f[13],
                    **{k: extra.get(k) for k in STATE_FIELDS}))
            elif f[0] == 'LIFECYCLE' and len(f) >= 6:
                life.append(dict(frame=int(f[1]), line=number, event=f[2],
                    relation=f[3], unit_id=int(f[4]), unit_type=f[5]))
            elif f[0] == 'LOSS' and len(f) >= 9:
                losses.append(dict(frame=int(f[1]), line=number, relation=f[2],
                    unit_id=int(f[3]), unit_type=f[4], x=int(f[5]), y=int(f[6]),
                    **parse_key_values(f[9:])))
            elif f[0] == 'HEALTH':
                health.append(dict(frame=int(f[1]), line=number, **parse_key_values(f[2:])))
    def first(predicate, source=states):
        return next((row for row in source if predicate(row)), None)
    enemy = first(lambda s: (s['enemyVisibleArmy'] or 0) > 0)
    deaths = [e for e in losses if e['relation'] == 'self']
    first_loss = deaths[0] if deaths else None
    important = set(range(0, min(states[-1]['frame'], 9600) + 1, 1200)) if states else set()
    for event in (enemy, first_loss):
        if event:
            important.update((event['frame'] - 240, event['frame'], event['frame'] + 240))
    milestones = []
    for frame in sorted(important):
        row = next((s for s in states if s['frame'] >= frame), None)
        if row and row not in milestones:
            milestones.append(row)
    notable = {'Protoss_Nexus', 'Protoss_Gateway', 'Protoss_Assimilator',
               'Protoss_Cybernetics_Core', 'Protoss_Dragoon', 'Protoss_Forge',
               'Protoss_Photon_Cannon', 'Protoss_Shield_Battery'}
    pre_contact = [s for s in states if enemy and s['frame'] < enemy['frame']]
    return dict(log=str(path.resolve()), log_sha256=sha256(path), match=match,
        first_visible_enemy_army=enemy, first_own_unit_death=first_loss,
        own_deaths_before_6000=dict(Counter(e['unit_type'] for e in deaths if e['frame'] < 6000)),
        loss_events=losses,
        initial_timeline=milestones,
        economy_before_contact=pre_contact[-1] if pre_contact else None,
        production_events=[e for e in life if e['relation'] == 'self'
            and e['unit_type'] in notable and e['event'] in ('create', 'complete', 'destroy')],
        health_at_milestones=[next((h for h in health if h['frame'] >= s['frame']), None)
                              for s in milestones],
        observed_enemy_units=dict(Counter(e['unit_type'] for e in life
            if e['relation'] == 'enemy' and e['event'] == 'discover')),
        note='Line references and legal-observation events support review; causal attribution is separate.')


def review(run):
    run = Path(run)
    checked = verify(run)
    result = inspect(run)
    rows = []
    for game in result['structurally_valid']:
        path = run / f"server/replays/bot-write/game-{game['game_id']}/Protodd/received/Protodd.log"
        if not path.exists():
            raise ValueError(f'missing archived telemetry for {game}')
        rows.append(dict(**game, **timeline(path)))
    return dict(schema='protodd-arena-loss-review-v1', campaign=str(run.resolve()),
        verification=checked, manifest_sha256=sha256(run/'manifest.json'),
        results_sha256=sha256(run/'server/results.jsonl'),
        source_sha256={str(p): sha256(p) for p in (Path(__file__), Path('training/arena.py'), Path('tools/log_analyzer.py'))},
        games=rows, excluded=result['excluded'], training_labels=False,
        strength_validated=False, final_test_opened=False)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    report = review(args.run)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps(dict(games=len(report['games']), excluded=report['excluded'], output=str(args.output))))
