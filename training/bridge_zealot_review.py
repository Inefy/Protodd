"""Review a source-pinned PvT Core-bridge pilot; never promote from two games."""
import argparse
import json
from pathlib import Path

from .arena import inspect, verify
from .schema import sha256


def trace(path):
    match = None
    state = None
    first_dragoon = None
    bridge_orders = 0
    errors = []
    enemy_activity = False
    for line in path.read_text(errors='replace').splitlines():
        fields = line.split(',')
        if fields[0] == 'MATCH':
            match = dict(part.split('=', 1) for part in fields[1:] if '=' in part)
        elif fields[0] == 'STATE':
            values = dict(part.split('=', 1) for part in fields[14:] if '=' in part)
            enemy_activity |= int(values.get('enemyVisibleArmy', 0)) > 0
            if fields[1] == '6000':
                state = dict(probes=int(values['probes'].split('/')[0]),
                             army=int(values['army']),
                             minerals=int(fields[7]), gas=int(fields[8]))
        elif fields[0] == 'MACRO' and len(fields) > 7:
            if fields[5] == 'issued-Dragoon' and first_dragoon is None:
                first_dragoon = int(fields[1])
            if (fields[5] == 'issued-Zealot' and
                    fields[7] == 'bridge Core warp-in with a second defender'):
                bridge_orders += 1
        elif fields[0] == 'ERROR':
            errors.append(line)
    return dict(match=match, state_6000=state, first_dragoon=first_dragoon,
                bridge_orders=bridge_orders, errors=errors, enemy_activity=enemy_activity,
                log=str(path.resolve()),
                log_sha256=sha256(path))


def campaign(path, games):
    path = Path(path)
    verify(path)
    inspection = inspect(path)
    manifest = json.loads((path / 'manifest.json').read_text())
    schedule = {row['gameID']: row for row in map(
        json.loads, (path / 'server/games.jsonl').read_text().splitlines())}
    rows = []
    for game in inspection['structurally_valid']:
        gid = game['game_id']
        log = path / f'server/replays/bot-write/game-{gid}/Protodd/received/Protodd.log'
        rows.append(dict(**game, **trace(log),
                         host=schedule[gid]['homeBot'] == 'Protodd',
                         map=schedule[gid]['map']))
    return dict(path=str(path.resolve()), manifest_sha256=sha256(path / 'manifest.json'),
                components=manifest['components'],
                healthy=(manifest['purpose'] == 'development' and
                         manifest['games'] == games and inspection['scheduled'] == games and
                         len(rows) == games and not inspection['excluded']),
                excluded=inspection['excluded'], rows=rows)


def review(plan_path, reference_path, candidate_path):
    plan = json.loads(Path(plan_path).read_text())
    n = plan['games_per_condition']
    reference = campaign(reference_path, n)
    candidate = campaign(candidate_path, n)
    ref = {x['game_id']: x for x in reference['rows']}
    cand = {x['game_id']: x for x in candidate['rows']}
    different = {'server/bots/Protodd/AI/Protodd.dll'}
    same_inputs = (reference['components'].keys() == candidate['components'].keys() and
                   all(reference['components'][key] == candidate['components'][key]
                       for key in reference['components'] if key not in different) and
                   reference['components']['server/bots/Protodd/AI/Protodd.dll'] ==
                   plan['reference_dll_sha256'] and
                   candidate['components']['server/bots/Protodd/AI/Protodd.dll'] ==
                   plan['candidate_dll_sha256'])
    paired = (reference['healthy'] and candidate['healthy'] and
              all(ref[i]['match'] == cand[i]['match'] and
                  ref[i]['host'] == cand[i]['host'] and ref[i]['map'] == cand[i]['map'] and
                  int(ref[i]['match']['seed']) == plan['seed_base'] + i
                  for i in range(n)))
    runtime = paired and all(x['enemy_activity'] and not x['errors'] and
                             x['state_6000'] is not None
                             for x in (*ref.values(), *cand.values()))
    checks = dict(healthy=reference['healthy'] and candidate['healthy'],
                  identical_inputs=same_inputs, paired=paired, runtime=runtime)
    if not all(checks.values()):
        return dict(schema='protodd-pvt-core-bridge-review-v1',
                    plan_sha256=sha256(plan_path), review_source_sha256=sha256(__file__),
                    reference=reference,
                    candidate=candidate, checks=checks, screen_more_games=False,
                    promotion_allowed=False)
    def mean(rows, key):
        return sum(row['state_6000'][key] for row in rows.values()) / n
    ref_dragoon = [x['first_dragoon'] for x in ref.values()]
    cand_dragoon = [x['first_dragoon'] for x in cand.values()]
    metrics = dict(reference_wins=sum(x['won'] for x in ref.values()),
                   candidate_wins=sum(x['won'] for x in cand.values()),
                   reference_army_6000=mean(ref, 'army'),
                   candidate_army_6000=mean(cand, 'army'),
                   reference_probes_6000=mean(ref, 'probes'),
                   candidate_probes_6000=mean(cand, 'probes'),
                   candidate_bridge_orders=sum(x['bridge_orders'] for x in cand.values()),
                   first_dragoon_reference=ref_dragoon,
                   first_dragoon_candidate=cand_dragoon)
    screen = (metrics['candidate_bridge_orders'] > 0 and
              metrics['candidate_army_6000'] >= metrics['reference_army_6000'] + 1 and
              metrics['candidate_probes_6000'] >= metrics['reference_probes_6000'] - 2 and
              metrics['candidate_wins'] >= metrics['reference_wins'] and
              all(a is not None and b is not None and b <= a + 240
                  for a, b in zip(ref_dragoon, cand_dragoon)))
    return dict(schema='protodd-pvt-core-bridge-review-v1',
                plan_sha256=sha256(plan_path), review_source_sha256=sha256(__file__),
                reference=reference,
                candidate=candidate, checks=checks, metrics=metrics,
                screen_more_games=screen, promotion_allowed=False,
                note='Two development pairs are a functional screen, not strength proof.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('plan', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('candidate', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = review(args.plan, args.reference, args.candidate)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({key: result.get(key) for key in
                      ('checks', 'metrics', 'screen_more_games')}))
