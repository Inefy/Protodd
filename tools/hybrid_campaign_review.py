"""Review matched local hybrid/shadow games without promoting a learned policy."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.arena import inspect, verify
from training.schema import sha256


def review(root):
    root = Path(root).resolve()
    result = dict(schema='protodd-hybrid-review-v1', strength_validated=False,
                  tournament_ready=False, training_labels_authorized=False, arms={})
    build = json.loads((root / 'ProtoddEvaluation.build.json').read_text())
    for arm in ('shadow', 'target'):
        run = root / arm
        verify(run)
        manifest = json.loads((run / 'manifest.json').read_text())
        classified = inspect(run)
        if len(classified['structurally_valid']) != manifest['games'] or classified['excluded']:
            raise ValueError(f'{arm}: incomplete or unhealthy paired results')
        dll = run / 'server/bots/Protodd/AI/Protodd.dll'
        if sha256(dll) != build['dll_sha256']:
            raise ValueError('evaluated DLL differs from build record')
        settings = json.loads((run / 'server/server_settings.json').read_text())
        reports = [json.loads(line) for line in (run / 'server/results.jsonl').read_text().splitlines()]
        schedule = {g['gameID']: g for g in map(json.loads, (run / 'server/games.jsonl').read_text().splitlines())}
        games = []
        for outcome in classified['structurally_valid']:
            gid = outcome['game_id']
            own = next(r for r in reports if r['gameID'] == gid and r['reportingBot'] == 'Protodd')
            if own['wasHost'] != (schedule[gid]['homeBot'] == 'Protodd'):
                raise ValueError('actual host does not match paired schedule')
            if outcome['frame'] >= settings['tournamentModuleSettings']['gameFrameLimit']:
                raise ValueError('frame-limited result is not a completed match')
            received = run / f'server/replays/bot-write/game-{gid}/Protodd/received'
            with (received / 'Protodd.log').open(newline='') as stream:
                rows = list(csv.reader(stream))
            fields = lambda row: dict(item.split('=', 1) for item in row if '=' in item)
            controller = fields(next(row for row in rows if row[0] == 'CONTROLLER'))
            if controller != dict(weights='1', control='1', mode='hybrid',
                                  hybridControl=str(int(arm == 'target'))):
                raise ValueError('wrong hybrid authority or missing loaded weights')
            summary = {k: int(v) for k, v in fields(next(row for row in rows
                                                       if row[0] == 'HYBRID_SUMMARY')).items()}
            performance = next(row for row in rows if row[0] == 'PERF_SUMMARY')
            errors = [row for row in rows if row[0] == 'ERROR']
            if errors or (received / 'WholeGame-model-error.txt').exists():
                raise ValueError('runtime/model error')
            if arm == 'shadow' and (summary['submitted'] or summary['accepted']):
                raise ValueError('shadow mode issued learned commands')
            maximum = Counter()
            observations = []
            with (received / 'WholeGame-observations.jsonl').open() as stream:
                for line in stream:
                    observation = json.loads(line)
                    observations.append(observation)
                    counts = Counter(e['type'] for e in observation['entities'] if e['relation'] == 0)
                    for kind, count in counts.items():
                        maximum[kind] = max(maximum[kind], count)
            if not observations or maximum[64] <= 5 or maximum[160] < 1:
                raise ValueError('native opening did not restore workers and production')
            with (received / 'WholeGame-inference.csv').open(newline='') as stream:
                inference = list(csv.reader(stream))
            if not inference or any(len(row) != 6 for row in inference):
                raise ValueError('missing sampled inference heartbeat')
            with (received / 'WholeGame-intents.csv').open(newline='') as stream:
                intents = Counter(row[2] for row in csv.reader(stream))
            opponent = run / f'server/replays/bot-write/game-{gid}/BananaBrain/received/Results_Protodd.txt'
            with opponent.open(newline='') as stream:
                opening = list(csv.reader(stream))[-1][5]
            if opening != 'PvP_3gaterobo':
                raise ValueError('opponent did not play frozen opening')
            games.append(dict(**outcome, was_host=own['wasHost'],
                              match=fields(next(row for row in rows if row[0] == 'MATCH')),
                              opponent_opening=opening, hybrid=summary,
                              max_probes=maximum[64], max_nexuses=maximum[154],
                              max_pylons=maximum[156], max_gateways=maximum[160],
                              max_zealots=maximum[65], max_dragoons=maximum[66],
                              peak_minerals=max(o['minerals'] for o in observations),
                              inference_samples=len(inference), intent_kinds=dict(intents),
                              max_callback_ms=float(performance[3]),
                              slow_callback_frames=sum(int(t.get('frameCount', 0)) for t in own.get('timers', [])),
                              evidence_sha256={name: sha256(received / name) for name in
                                  ('Protodd.log', 'WholeGame-inference.csv', 'WholeGame-intents.csv',
                                   'WholeGame-observations.jsonl')}))
        result['arms'][arm] = dict(dll_sha256=sha256(dll), manifest_sha256=sha256(run / 'manifest.json'),
                                   wins=sum(g['won'] for g in games), games=games)
    shadow, target = (result['arms'][name]['games'] for name in ('shadow', 'target'))
    if len(shadow) != len(target) or any(s['match'] != t['match'] or s['was_host'] != t['was_host']
                                       for s, t in zip(shadow, target)):
        raise ValueError('actual seeds/maps differ between arms')
    settings = [json.loads((root / arm / 'server/server_settings.json').read_text()) for arm in ('shadow', 'target')]
    for value in settings:
        value.pop('serverPort')
    if settings[0] != settings[1]:
        raise ValueError('server comparison differs beyond port')
    manifests = [json.loads((root / arm / 'manifest.json').read_text())['components'] for arm in ('shadow', 'target')]
    allowed = {'server/server_settings.json', 'client1/client_settings.json', 'client2/client_settings.json',
               'server/bots/Protodd/read/WholeGame-hybrid-mode.txt'}
    if any(manifests[0].get(name) != manifests[1].get(name)
           for name in set(manifests[0]) | set(manifests[1]) if name not in allowed):
        raise ValueError('frozen campaign inputs differ beyond mode and connection settings')
    result['matched_actual_maps_and_seeds'] = True
    result['strict_runtime_gate_pass'] = all(g['max_callback_ms'] < 42 and g['slow_callback_frames'] == 0
                                             for arm in result['arms'].values() for g in arm['games'])
    result['learned_target_exposure'] = sum(g['hybrid']['accepted'] for g in target)
    result['interpretation'] = ('No learned commands were accepted; strength cannot be attributed to the model.'
                                if not result['learned_target_exposure'] else
                                'Exploratory target exposure only; two games do not establish general strength.')
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('root', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = review(args.root)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(arms={arm: value['wins'] for arm, value in report['arms'].items()},
                          learned_target_exposure=report['learned_target_exposure'],
                          interpretation=report['interpretation']), indent=2))
