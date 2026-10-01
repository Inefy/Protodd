"""Compare ladder all-in build requests on both winning and losing sides.

Uses train and unassigned raw replays, never reserved validation/test groups.
Requests are not proof of completed production. Outcomes remain claimed/inferred.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
from pathlib import Path
import sqlite3
import subprocess
import time

from .mine_allins import summarize
from .replay_cohort import stable
from .schema import sha256


def request_profile(commands, player):
    buildings = defaultdict(set)
    trained = Counter()
    first = {}
    events = []
    kinds = {64: 'probe', 65: 'zealot', 66: 'dragoon', 61: 'dark_templar', 83: 'reaver',
             154: 'nexus', 155: 'robotics_facility', 156: 'pylon', 157: 'assimilator',
             159: 'observatory', 160: 'gateway', 163: 'citadel_of_adun', 164: 'cybernetics_core',
             165: 'templar_archives', 171: 'robotics_support_bay'}
    recent = {}
    for command in commands:
        if command['PlayerID'] != player['ID'] or command['Frame'] > 23040:
            continue
        name = command['Type']['Name']
        unit = command.get('Unit', {}).get('ID')
        if unit not in kinds or name not in ('Build', 'Train'):
            continue
        kind = kinds[unit]
        frame = command['Frame']
        if name == 'Build':
            position = command.get('Pos', {})
            key = (position.get('X'), position.get('Y'))
            if None in key or key in buildings[kind]:
                continue
            buildings[kind].add(key)
            count = len(buildings[kind]) + int(kind == 'nexus')
        else:
            # Rapid command repeats are one intent, not additional completed units.
            key = (kind, command.get('Producer', command.get('UnitTag')))
            if frame - recent.get(key, -1000) < 24:
                continue
            recent[key] = frame
            trained[kind] += 1
            count = trained[kind]
        first.setdefault(f'{kind}_{count}', frame)
        if len(events) < 35:
            events.append(dict(frame=frame, request=name, kind=kind, ordinal=count))
    absent = 10**9
    at = lambda key: first.get(key, absent)
    nexus = at('nexus_2')
    choices = []
    if (at('gateway_2') <= 4200 and at('gateway_2') < min(at('assimilator_1'), at('cybernetics_core_1'))
            and nexus > at('gateway_2') + 480):
        choices.append(('gasless-two-gate-zealot', at('gateway_2')))
    if at('templar_archives_1') <= 7500 and nexus > at('templar_archives_1') + 480:
        choices.append(('one-base-dt', at('templar_archives_1')))
    if at('robotics_support_bay_1') <= 8500 and nexus > at('robotics_support_bay_1') + 480:
        choices.append(('one-base-reaver', at('robotics_support_bay_1')))
    # Higher gateway counts specify the committed production ceiling.
    if at('dragoon_1') <= 7200:
        if at('gateway_4') <= 9600 and nexus > at('gateway_4') + 480:
            choices.append(('four-gate-dragoon', at('gateway_4')))
        elif at('gateway_3') <= 8400 and nexus > at('gateway_3') + 480:
            choices.append(('three-gate-dragoon', at('gateway_3')))
    elif at('gateway_4') <= 9000 and nexus > at('gateway_4') + 480:
        choices.append(('four-gate-zealot', at('gateway_4')))
    if not choices:
        return None
    family, commitment = min(choices, key=lambda item: item[1])
    return dict(family=family, commitment_frame=commitment,
                ready_frame=None, workers_at_commitment=4 + sum(
                    frame <= commitment for key, frame in first.items() if key.startswith('probe_')),
                first=first, request_sequence=events)


def inspect_replay(record, root, parser):
    path = (root / record['path']).resolve()
    if not path.is_relative_to(root) or sha256(path) != record['sha256']:
        raise ValueError('raw replay differs from audited identity')
    parsed = json.loads(subprocess.run([str(parser), '-indent=false', '-cmds', str(path)],
        check=True, capture_output=True, timeout=30,
        creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout)
    players = [p for p in parsed['Header']['Players'] if p['Type']['ID'] == 2 and not p.get('Observer')]
    if len(players) != 2 or len({p['Team'] for p in players}) != 2 or parsed['Commands'].get('ParseErrCmds'):
        return []
    computed = parsed['Computed']
    descriptions = {p['PlayerID']: p for p in computed['PlayerDescs']}
    leaves = [c for c in (computed.get('LeaveGameCmds') or []) if c['PlayerID'] in descriptions]
    winner = None
    if len(leaves) == 1 and leaves[0]['Reason']['ID'] == 1:
        remaining = next(p for p in players if p['ID'] != leaves[0]['PlayerID'])
        if parsed['Header']['Frames'] - leaves[0]['Frame'] <= 240 and descriptions[remaining['ID']]['LastCmdFrame'] > leaves[0]['Frame']:
            winner = remaining['SlotID']
    source_race = record.get('source_record', {}).get('winnerRace')
    rows = []
    for player in players:
        if player['Race']['Letter'] != ord('P'):
            continue
        profile = request_profile(parsed['Commands']['Cmds'], player)
        if not profile:
            continue
        inferred = winner == player['SlotID'] if winner is not None else None
        races = {chr(p['Race']['Letter']) for p in players}
        claimed = source_race == 'P' if len(races) == 2 and source_race in races else None
        won = claimed if claimed is not None else inferred
        source = 'source_winner_race_claim' if claimed is not None else 'single_quit_inference' if inferred is not None else 'unknown'
        if inferred is not None and claimed is not None and inferred != claimed:
            won, source = None, 'conflicting_claim_and_quit'
        enemy = next(p for p in players if p != player)
        enemy_start = descriptions[enemy['ID']].get('StartLocation')
        home = descriptions[player['ID']].get('StartLocation')
        forward = None
        for command in parsed['Commands']['Cmds']:
            position = command.get('Pos', {})
            if (command['PlayerID'] == player['ID'] and profile['commitment_frame'] <= command['Frame'] <= 12000
                    and command.get('Order', {}).get('Name') in ('AttackMove', 'AttackUnit')
                    and home and enemy_start and 'X' in position):
                distance = lambda start: (position['X'] - start['X'])**2 + (position['Y'] - start['Y'])**2
                if distance(enemy_start) <= 1536**2 and distance(enemy_start) < distance(home):
                    forward = command['Frame']
                    break
        rows.append(dict(**profile, game_id=record['group'], path=record['path'],
            replay_sha256=record['sha256'], perspective=player['SlotID'],
            matchup='Pv' + chr(enemy['Race']['Letter']), won=won, outcome_source=source,
            forward_attack_frame=forward))
    return rows


def mine(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    cohort = json.loads(args.cohort.read_text())
    groups = {g['duplicate_group']: g['split'] for g in cohort['games']}
    connection = sqlite3.connect(args.audit.resolve().as_uri() + '?mode=ro', uri=True)
    selected = {}
    excluded = Counter()
    try:
        for detail, in connection.execute("SELECT detail FROM replays WHERE status='parsed' ORDER BY path"):
            record = json.loads(detail)
            players = record['players']
            if len(players) != 2 or record['frames'] < 4320:
                excluded['too_short_or_wrong_players'] += 1
                continue
            timestamp = datetime.fromisoformat(record['start_time']).timestamp()
            group = stable([record['map_sha256'], timestamp,
                            sorted((p['name'].casefold(), p['race']) for p in players)])
            if groups.get(group) in ('validation', 'test'):
                excluded['reserved_group'] += 1
                continue
            # The qualified player may be our opponent; retain losing Protoss
            # sides as descriptive evidence, never as qualified imitation rows.
            if not any(any((c.get('mmr_claim') or 0) >= 2000 for c in p.get('source_claims', [])) for p in players):
                excluded['no_matched_mmr_2000_claim'] += 1
                continue
            record['group'] = group
            if group in selected:
                excluded['duplicate_recording'] += 1
            else:
                selected[group] = record
    finally:
        connection.close()
    started = time.monotonic()
    rows = []
    print(json.dumps(dict(selected_games=len(selected), excluded=dict(excluded))), flush=True)
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for index, profiles in enumerate(pool.map(lambda r: inspect_replay(r, args.replays.resolve(), args.parser.resolve()), selected.values())):
            rows.extend(profiles)
            if index % 500 == 0:
                print(json.dumps(dict(parsed=index, total=len(selected), commitments=len(rows))), flush=True)
    report = dict(schema='protodd-ladder-allin-requests-v1', complete=True,
        source_sha256=sha256(Path(__file__)), cohort_sha256=sha256(args.cohort), audit_sha256=sha256(args.audit),
        parser_sha256=sha256(args.parser), selected_games=len(selected), excluded=dict(excluded),
        reserved_groups_opened=0, elapsed_seconds=time.monotonic() - started,
        families=summarize(rows), strength_validated=False,
        limitations=['Raw requests, not accepted/completed production.',
                     'Winning and losing Protoss sides included; some acting-player MMR is unknown.',
                     'Winner race source claims and conservative quit inferences are not authoritative results.',
                     'Descriptive observational rankings; require fresh bot games before strength claims.'])
    (output / 'profiles.jsonl').write_text(''.join(json.dumps(r) + '\n' for r in rows))
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps([{k: f[k] for k in ('matchup', 'family', 'perspectives', 'known_outcomes', 'wins', 'wilson_95')}
                      for f in report['families']], indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('cohort', 'audit', 'replays', 'parser', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--workers', type=int, default=2)
    args = parser.parse_args()
    if not 1 <= args.workers <= 4:
        parser.error('one to four workers required')
    mine(args)
