"""Audit replay-derived opening pilots without treating a launch as a win."""
import argparse
from collections import Counter
import csv
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.arena import inspect, verify
from training.schema import sha256


def opening_observations(path):
    maximum = Counter()
    first = {}
    home = None
    forward = None
    with Path(path).open() as stream:
        for line in stream:
            observation = json.loads(line)
            own = [e for e in observation['entities'] if e['relation'] == 0]
            if home is None:
                home = next((e['position'] for e in own if e['type'] == 154), None)
            counts = Counter(e['type'] for e in own if e['completed'])
            for kind, count in counts.items():
                maximum[kind] = max(maximum[kind], count)
                for ordinal in (1, 2, 4, 6):
                    if count >= ordinal:
                        first.setdefault(f'{kind}_{ordinal}', observation['frame'])
            if home and forward is None:
                advanced = sum(e['completed'] and e['type'] in (65, 66, 61) and
                               sum((a - b)**2 for a, b in zip(e['position'], home)) >= 1200**2
                               for e in own)
                if advanced >= 4:
                    forward = observation['frame']
    return dict(max_completed=dict(maximum), first_completed=first,
                first_four_fighters_1200_from_home=forward)


def review(run, build_record):
    run = Path(run).resolve()
    verified = verify(run)
    classified = inspect(run)
    if len(classified['structurally_valid']) != verified['games'] or classified['excluded']:
        raise ValueError('incomplete, crashed or timed-out opening campaign')
    build = json.loads(Path(build_record).read_text())
    if sha256(run / 'server/bots/Protodd/AI/Protodd.dll') != build['dll_sha256']:
        raise ValueError('campaign DLL differs from recorded build')
    expected = (run / 'server/bots/Protodd/read/AllIn-opening.txt').read_text().strip()
    settings = json.loads((run / 'server/server_settings.json').read_text())
    reports = [json.loads(line) for line in (run / 'server/results.jsonl').read_text().splitlines()]
    schedule = {r['gameID']: r for r in map(json.loads, (run / 'server/games.jsonl').read_text().splitlines())}
    games = []
    for outcome in classified['structurally_valid']:
        gid = outcome['game_id']
        own = next(r for r in reports if r['gameID'] == gid and r['reportingBot'] == 'Protodd')
        if own['wasHost'] != (schedule[gid]['homeBot'] == 'Protodd'):
            raise ValueError('host differs from frozen schedule')
        if outcome['frame'] >= settings['tournamentModuleSettings']['gameFrameLimit']:
            raise ValueError('frame-limited outcome is not a completed match')
        received = run / f'server/replays/bot-write/game-{gid}/Protodd/received'
        with (received / 'Protodd.log').open(newline='') as stream:
            rows = list(csv.reader(stream))
        fields = lambda row: dict(item.split('=', 1) for item in row if '=' in item)
        selected = next(row[1] for row in rows if row[0] == 'ALLIN_SELECTION')
        if selected != expected:
            raise ValueError('DLL ignored frozen opening profile')
        controller = fields(next(row for row in rows if row[0] == 'CONTROLLER'))
        if controller != dict(weights='1', control='1', mode='hybrid', hybridControl='1'):
            raise ValueError('pilot did not use the trained hybrid')
        if any(row[0] == 'ERROR' for row in rows) or (received / 'WholeGame-model-error.txt').exists():
            raise ValueError('runtime error')
        phases = []
        for row in rows:
            if row[0] != 'ALLIN':
                continue
            event = dict(frame=int(row[1]), **fields(row))
            if not phases or event['phase'] != phases[-1]['phase']:
                phases.append(event)
        performance = next(row for row in rows if row[0] == 'PERF_SUMMARY')
        hybrid = {k: int(v) for k, v in fields(next(row for row in rows
                      if row[0] == 'HYBRID_SUMMARY')).items()}
        opponent = run / f'server/replays/bot-write/game-{gid}/BananaBrain/received/Results_Protodd.txt'
        with opponent.open(newline='') as stream:
            enemy_opening = list(csv.reader(stream))[-1][5]
        if enemy_opening != 'PvP_3gaterobo':
            raise ValueError('opponent changed its frozen opening')
        games.append(dict(**outcome, was_host=own['wasHost'],
            match=fields(next(row for row in rows if row[0] == 'MATCH')),
            opponent_opening=enemy_opening, phases=phases,
            hybrid=hybrid,
            **opening_observations(received / 'WholeGame-observations.jsonl'),
            max_callback_ms=float(performance[3]),
            strict_42ms_callback_gate_passed=float(performance[3]) <= 42,
            evidence_sha256={name: sha256(received / name) for name in
                ('Protodd.log', 'WholeGame-observations.jsonl')}))
    return dict(schema='protodd-allin-campaign-review-v1', opening=expected,
                review_source_sha256=sha256(Path(__file__)),
                dll_sha256=build['dll_sha256'], strength_validated=False,
                tournament_ready=False, wins=sum(g['won'] for g in games), games=games)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--build-record', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    result = review(args.run, args.build_record)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
