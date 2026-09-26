"""Build causal matched training contexts from adjudicated worker-game pilots.

Never fit from a single small pilot or use outcomes from non-normal games.
"""
import argparse
import json
from pathlib import Path

from .arena import verify
from .schema import sha256

FEATURES = ('probes', 'army', 'minerals', 'gas', 'supply_used', 'supply_total',
            'nexuses', 'gateways', 'zealots', 'dragoons', 'enemy_visible_army')
CONTEXT_FRAME = 2160  # Strictly before the first worker intervention at 2400.


def context(log):
    found = []
    for line in log.read_text(errors='replace').splitlines():
        fields = line.split(',')
        if fields[0] != 'STATE' or int(fields[1]) != CONTEXT_FRAME:
            continue
        data = dict(item.split('=', 1) for item in fields[14:] if '=' in item)
        get = lambda name: int(data[name].split('/')[0])
        found.append(dict(probes=get('probes'), army=get('army'),
                          minerals=int(fields[7]), gas=int(fields[8]),
                          supply_used=int(fields[9]), supply_total=int(fields[10]),
                          nexuses=get('nexuses'), gateways=get('gateways'),
                          zealots=get('zealots'), dragoons=get('dragoons'),
                          enemy_visible_army=get('enemyVisibleArmy')))
    if len(found) != 1:
        raise ValueError('missing or repeated pre-intervention state')
    return found[0]


def build(reports):
    examples = []
    seen = set()
    source = {}
    for path in reports:
        report = json.loads(path.read_text())
        if (report.get('schema') != 'protodd-worker-outcome-pilot-v1' or
                report.get('usable_training_outcomes') is not True or
                any(report.get('checks', {}).get(name) is not True for name in
                    ('healthy', 'matched', 'identical_inputs', 'runtime')) or
                report.get('promotion_eligible') is not False):
            raise ValueError('unqualified worker training outcomes')
        source[str(path.resolve())] = sha256(path)
        conditions = {}
        for profile, campaign in report['campaigns'].items():
            run = Path(campaign['campaign'])
            verify(run)
            if sha256(run/'manifest.json') != campaign['manifest_sha256']:
                raise ValueError('campaign manifest changed')
            conditions[profile] = {}
            for game in campaign['games']:
                log = Path(game['log'])
                if sha256(log) != game['log_sha256']:
                    raise ValueError('training episode log changed')
                conditions[profile][game['game_id']] = dict(game=game, context=context(log))
        baseline = conditions['baseline']
        for profile in ('plus-one', 'plus-two'):
            for gid, treatment in conditions[profile].items():
                reference = baseline[gid]
                a, b = reference['game'], treatment['game']
                key = (a['opponent'], a['map'], a['match']['seed'], a['host'])
                if (key, profile) in seen:
                    raise ValueError('duplicate worker training seed or profile')
                seen.add((key, profile))
                if (a['match'] != b['match'] or a['host'] != b['host'] or not b['exposed']):
                    continue
                same_pre_state = reference['context'] == treatment['context']
                examples.append(dict(profile=profile, opponent=a['opponent'], map=a['map'],
                                     seed=int(a['match']['seed']), host=a['host'],
                                     features=reference['context'],
                                     candidate_pre_state=treatment['context'],
                                     same_pre_state=same_pre_state,
                                     fit_eligible=same_pre_state,
                                     win_delta=int(b['won'])-int(a['won']),
                                     baseline_win=a['won'], intervention_win=b['won'],
                                     baseline_log_sha256=a['log_sha256'],
                                     intervention_log_sha256=b['log_sha256']))
    eligible = [x for x in examples if x['fit_eligible']]
    distinct_contexts = {(x['opponent'], x['map'], x['seed'], x['host']) for x in eligible}
    nonzero = [x['win_delta'] for x in eligible if x['win_delta']]
    ready = len(distinct_contexts) >= 24 and len(eligible) >= 48 and nonzero.count(1) >= 3 and nonzero.count(-1) >= 3
    return dict(schema='protodd-worker-outcome-training-dataset-v1', context_frame=CONTEXT_FRAME,
                features=FEATURES, sources=source, examples=examples,
                counts=dict(examples=len(examples), distinct_contexts=len(distinct_contexts),
                            exact_pre_states=len(eligible),
                            positive_differences=nonzero.count(1), negative_differences=nonzero.count(-1)),
                ready_for_bounded_fit=ready, training_only=True,
                final_test_opened=False, promotion_eligible=False,
                note='Outcome differences are observational paired training labels. Seed matching reduces noise but does not guarantee causal equivalence.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    parser.add_argument('reports', type=Path, nargs='+')
    args = parser.parse_args()
    if args.output.exists():
        raise FileExistsError(args.output)
    result = build(args.reports)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps(dict(counts=result['counts'], ready_for_bounded_fit=result['ready_for_bounded_fit'])))
