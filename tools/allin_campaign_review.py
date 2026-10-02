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


def banana_evidence(run, gid, outcome=None, map_name=None):
    received = run / f'server/replays/bot-write/game-{gid}/BananaBrain/received'
    result = received / 'Results_Protodd.txt'
    with result.open(newline='') as stream:
        rows = list(csv.reader(stream))
    if len(rows) != 1 or len(rows[0]) < 13:
        raise ValueError('missing or ambiguous BananaBrain game record')
    opening = rows[0][5]
    if outcome is not None and (rows[0][4] != map_name or
            abs(int(rows[0][8]) - outcome['frame']) > 120 or
            rows[0][12] != ('0' if outcome['won'] else '1')):
        raise ValueError('BananaBrain game record differs from paired result')
    frozen = None
    for directory in ('AI', 'read'):
        config = run / f'server/bots/BananaBrain/{directory}/Configuration.txt'
        if config.exists():
            for line in config.read_text().splitlines():
                key, sep, value = line.partition('=')
                if sep and key.strip() == 'PvP_opening':
                    frozen = value.strip()
    if frozen and opening != frozen:
        raise ValueError('BananaBrain changed its frozen opening')
    return dict(opening=opening, frozen_opening=frozen, evidence_sha256={result.name: sha256(result)})


def pluto_evidence(run, gid, outcome, map_name):
    received = run / f'server/replays/bot-write/game-{gid}/Pluto/received'
    record = received / 'pluto_bandit_Protodd.txt'
    lines = record.read_text().splitlines()
    if not lines or lines[-1] != 'end':
        raise ValueError('unfinished Pluto game record')
    records = [json.loads(line) for line in lines if line.startswith('{')]
    starts = [r for r in records if r.get('t') == 'start']
    ends = [r for r in records if r.get('t') == 'end']
    if len(starts) != 1 or len(ends) != 1:
        raise ValueError('missing or ambiguous Pluto game boundaries')
    start, end = starts[0], ends[0]
    events = [r for r in records if r.get('t') == 'event']
    if any(r.get('t') not in ('start', 'end', 'event') for r in records) or any(
            e.get('what') not in ('latency_mismatch', 'machine_slow', 'resign') for e in events):
        raise ValueError('unreviewed Pluto engine error/event')
    if (start.get('opp') != 'Protodd' or end.get('opp') != 'Protodd' or
            start.get('id') != end.get('id') or any(e.get('id') != start.get('id') for e in events) or
            start.get('map') != map_name or
            start.get('own_race') != 'P' or start.get('opp_race') != 'P' or
            start.get('mode') != 'block' or start.get('budget_ms') != 40 or
            start.get('model') != 'md07x02_cog2026_2578600_int8mv' or
            start.get('fps') != 6 or end.get('steps', 0) <= 0 or
            end.get('result') != ('loss' if outcome['won'] else 'win') or
            abs(end.get('frames', -999) - outcome['frame']) > 120):
        raise ValueError('Pluto game/settings do not match completed result')
    logs = {name: (received / name).read_text(errors='replace')
            for name in ('pluto.log', 'pluto_infer.log')}
    if ('inference server up' not in logs['pluto.log'] or 'onEnd' not in logs['pluto.log'] or
            '[pluto_infer] ready:' not in logs['pluto_infer.log'] or
            any(token in text.lower() for text in logs.values()
                for token in ('quitting in', 'fatal', 'engine died', 'cannot start'))):
        raise ValueError('Pluto engine did not finish healthy')
    return dict(opening=start['bo'], start=start, end=end, events=events,
                evidence_sha256={name: sha256(received / name)
                                 for name in (record.name, 'pluto.log', 'pluto_infer.log')})


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
        if outcome['opponent'] == 'BananaBrain':
            opponent_evidence = banana_evidence(run, gid, outcome, schedule[gid]['map'])
        elif outcome['opponent'] == 'Pluto':
            opponent_evidence = pluto_evidence(run, gid, outcome, schedule[gid]['map'])
        else:
            raise ValueError('opponent requires a dedicated health evidence parser')
        games.append(dict(**outcome, was_host=own['wasHost'],
            match=fields(next(row for row in rows if row[0] == 'MATCH')),
            opponent_opening=opponent_evidence['opening'], opponent_evidence=opponent_evidence, phases=phases,
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
