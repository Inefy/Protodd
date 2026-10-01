"""Mine early one-base ladder commitments from frozen training tensors.

Replay outcomes are source claims or conservative quit-based inferences, not
authoritative game results. Build families are descriptive, not causal rankings.
Final-test sequences are never read. No model training or deployment occurs.
"""
import argparse
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
import json
import math
from pathlib import Path
import sqlite3
import statistics
import subprocess
import time

import numpy as np

from .schema import sha256
from .shards import TensorShards

KINDS = ('probe', 'nexus', 'gateway', 'assimilator', 'cybernetics_core',
         'zealot', 'dragoon', 'dark_templar', 'reaver', 'robotics_facility',
         'citadel_of_adun', 'templar_archives', 'forge', 'photon_cannon')


def wilson(wins, total):
    if not total:
        return [0.0, 1.0]
    z = 1.959963984540054
    rate = wins / total
    denominator = 1 + z * z / total
    center = (rate + z * z / (2 * total)) / denominator
    width = z * math.sqrt(rate * (1 - rate) / total + z * z / (4 * total * total)) / denominator
    return [center - width, center + width]


def classify(first, workers):
    """Use committed infrastructure, not a win or eventual surviving army."""
    absent = 10**9
    at = lambda key: first.get(key, absent)
    nexus = at('nexus_2')
    choices = [
        ('one-base-dt', at('dark_templar_1'), 10000),
        ('one-base-reaver', at('reaver_1'), 11000),
        ('four-gate-dragoon', max(at('gateway_4'), at('dragoon_2')), 9600),
        ('three-gate-dragoon', max(at('gateway_3'), at('dragoon_2')), 8400),
        ('four-gate-zealot', max(at('gateway_4'), at('zealot_2')), 9000),
    ]
    for family, frame, maximum in choices:
        if frame <= maximum and nexus > frame + 480 and workers(frame) <= 28:
            return family, frame
    gate = at('gateway_2')
    if (gate <= 4200 and gate < min(at('assimilator_1'), at('cybernetics_core_1')) and
            nexus > gate + 480 and workers(gate) <= 18):
        return 'gasless-two-gate-zealot', gate
    return None


def describe(features, metadata, schema):
    indices = {item['name']: i for i, item in enumerate(schema['features'])}
    frames = metadata['frames']
    first = {}
    counts = {}
    for kind in KINDS:
        complete = np.rint(features[:, indices[f'own_complete/{kind}']] * 50).astype(int)
        started = complete + np.rint(features[:, indices[f'own_incomplete/{kind}']] * 50).astype(int)
        values = complete if kind in ('zealot', 'dragoon', 'dark_templar', 'reaver') else started
        counts[kind] = values
        thresholds = (1, 2, 3, 4) if kind in ('gateway', 'nexus') else (1, 2, 4, 6, 8)
        for number in thresholds:
            hits = np.flatnonzero(values >= number)
            if len(hits):
                first[f'{kind}_{number}'] = int(frames[hits[0]])
    def workers(frame):
        return int(counts['probe'][max(0, np.searchsorted(frames, frame, side='right') - 1)])
    family = classify(first, workers)
    if not family:
        return None
    name, commitment = family
    ready_key = ('dark_templar_1' if name == 'one-base-dt' else 'reaver_1' if name == 'one-base-reaver'
                 else 'dragoon_4' if 'dragoon' in name else 'zealot_4')
    ready = first.get(ready_key)
    end = min(12000, first.get('nexus_2', 12000))
    peak = {kind: int(values[frames <= end].max(initial=0)) for kind, values in counts.items()}
    return dict(family=name, commitment_frame=commitment, ready_frame=ready,
                workers_at_commitment=workers(commitment), workers_at_ready=workers(ready) if ready else None,
                first=first, peak_before_second_base=peak)


def replay_facts(game, root, parser):
    path = (root / game['path']).resolve()
    if not path.is_relative_to(root) or sha256(path) != game['replay_sha256']:
        raise ValueError('replay identity differs from training release')
    process = subprocess.run([str(parser), '-indent=false', '-cmds', str(path)],
                             capture_output=True, timeout=30, check=True,
                             creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0))
    parsed = json.loads(process.stdout)
    players = [p for p in parsed['Header']['Players'] if p['Type']['ID'] == 2 and not p.get('Observer')]
    if len(players) != 2 or len({p['Team'] for p in players}) != 2:
        raise ValueError('replay is not two opposing humans')
    computed = parsed['Computed']
    descriptions = {p['PlayerID']: p for p in computed['PlayerDescs']}
    leaves = [c for c in (computed.get('LeaveGameCmds') or []) if c['PlayerID'] in descriptions]
    # Do not turn disconnects, drops, missing quits, or two-player departures into wins.
    inferred_winner = None
    if len(leaves) == 1 and leaves[0]['Reason']['ID'] == 1:
        remaining = next(p for p in players if p['ID'] != leaves[0]['PlayerID'])
        if (parsed['Header']['Frames'] - leaves[0]['Frame'] <= 240 and
                descriptions[remaining['ID']]['LastCmdFrame'] > leaves[0]['Frame']):
            inferred_winner = remaining['SlotID']
    attacks = defaultdict(list)
    for command in parsed['Commands']['Cmds']:
        if command['Frame'] > 12000 or command.get('Order', {}).get('Name') not in ('AttackMove', 'AttackUnit'):
            continue
        actor = next((p for p in players if p['ID'] == command['PlayerID']), None)
        if actor is None:
            continue
        enemy = next(p for p in players if p != actor)
        destination = command.get('Pos', {})
        start = descriptions[enemy['ID']].get('StartLocation')
        home = descriptions[actor['ID']].get('StartLocation')
        if start and home and 'X' in destination:
            enemy_distance = (destination['X'] - start['X'])**2 + (destination['Y'] - start['Y'])**2
            home_distance = (destination['X'] - home['X'])**2 + (destination['Y'] - home['Y'])**2
            if enemy_distance <= 1536**2 and enemy_distance < home_distance:
                attacks[actor['SlotID']].append(command['Frame'])
    return dict(replay_sha256=game['replay_sha256'], inferred_winner_slot=inferred_winner,
                forward_attack_intent_frames=dict(attacks), players=[dict(slot=p['SlotID'], race=chr(p['Race']['Letter']))
                                                                     for p in players])


def summarize(rows):
    groups = defaultdict(list)
    for row in rows:
        groups[(row['matchup'], row['family'])].append(row)
    result = []
    for (matchup, family), values in groups.items():
        known = [row for row in values if row['won'] is not None]
        wins = sum(row['won'] for row in known)
        median = lambda entries: statistics.median(entries) if entries else None
        timing_keys = sorted({key for row in values for key in row['first']})
        ready = [row['ready_frame'] for row in values if row['ready_frame'] is not None]
        forward = [row for row in values if row['forward_attack_frame'] is not None]
        result.append(dict(matchup=matchup, family=family, perspectives=len(values),
            unique_games=len({r['game_id'] for r in values}), known_outcomes=len(known), wins=wins,
            inferred_or_claimed_win_rate=wins / len(known) if known else None, wilson_95=wilson(wins, len(known)),
            outcome_sources=dict(Counter(row['outcome_source'] for row in values)),
            ready_observed=len(ready), median_ready_frame=median(ready),
            median_commitment_frame=median([row['commitment_frame'] for row in values]),
            median_workers_at_commitment=median([row['workers_at_commitment'] for row in values]),
            forward_attack_observed=len(forward),
            median_forward_attack_frame=median([row['forward_attack_frame'] for row in forward]),
            median_first_frames={key: median([row['first'][key] for row in values if key in row['first']])
                                 for key in timing_keys},
            transition_second_base_observed=sum('nexus_2' in row['first'] for row in values),
            examples=[dict(game_id=r['game_id'], path=r['path'], perspective=r['perspective'], won=r['won'],
                           first=r['first'], forward_attack_frame=r['forward_attack_frame'])
                      for r in sorted(values, key=lambda r: (abs((r['ready_frame'] or 12000) - (median(ready) or 12000)),
                                                             r['game_id']))[:4]]))
    return sorted(result, key=lambda r: (r['matchup'], -r['wilson_95'][0], -r['known_outcomes']))


def mine(args):
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    started = time.monotonic()
    rows = []
    with TensorShards(args.tensors, verify_hashes=True) as dataset:
        selected = [sequence for sequence in dataset.sequences if sequence['split'] == 'train']
        games = dataset.games
        for index, sequence in enumerate(selected):
            batch = dataset.read_rows(sequence['start'], min(sequence['count'], 16 * 60 + 1))
            # Only the first 16 game minutes; every loaded row is from train.
            mask = batch['frames'] <= 16 * 60 * 24
            description = describe(batch['features'][mask],
                                   {key: batch[key][mask] for key in ('frames',)}, dataset.manifest['schema'])
            if description:
                game = games[sequence['game_index']]
                rows.append(dict(**description, game_id=game['game_id'], path=game['path'],
                    replay_sha256=game['replay_sha256'], matchup=sequence['matchup'],
                    perspective=sequence['perspective'], slot=game['slots'][sequence['perspective']]))
            if index % 500 == 0:
                print(json.dumps(dict(stage='tensor_profiles', scanned=index, total=len(selected), candidates=len(rows))), flush=True)
        provenance = dict(tensor_manifest_sha256=dataset.identity,
                          training_perspectives_scanned=len(selected),
                          training_unique_games=len({s['game_index'] for s in selected}),
                          validation_sequences_opened=0, final_test_sequences_opened=0)
        chosen_games = {row['game_id']: next(g for g in games if g['game_id'] == row['game_id']) for row in rows}
    root = args.replays.resolve()
    parser = args.parser.resolve()
    facts = {}
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for index, (game, fact) in enumerate(zip(chosen_games.values(), pool.map(
                lambda game: replay_facts(game, root, parser), chosen_games.values()))):
            facts[game['game_id']] = fact
            if index % 100 == 0:
                print(json.dumps(dict(stage='candidate_replays', parsed=index, total=len(chosen_games))), flush=True)
    connection = sqlite3.connect(args.audit.resolve().as_uri() + '?mode=ro', uri=True)
    try:
        for row in rows:
            game = chosen_games[row['game_id']]
            detail = connection.execute('SELECT detail FROM replays WHERE path=? AND sha256=?',
                                         (game['path'], game['replay_sha256'])).fetchone()
            if detail is None:
                raise ValueError('missing source metadata binding')
            metadata = json.loads(detail[0])
            source_race = metadata.get('source_record', {}).get('winnerRace')
            claimed = source_race == 'P' if row['matchup'] != 'PvP' and source_race in ('P', 'T', 'Z') else None
            fact = facts[row['game_id']]
            inferred = fact['inferred_winner_slot'] == row['slot'] if fact['inferred_winner_slot'] is not None else None
            if claimed is not None and inferred is not None and claimed != inferred:
                row.update(won=None, outcome_source='conflicting_claim_and_quit')
            elif claimed is not None:
                row.update(won=claimed, outcome_source='source_winner_race_claim')
            elif inferred is not None:
                row.update(won=inferred, outcome_source='conservative_single_quit_inference')
            else:
                row.update(won=None, outcome_source='unknown')
            attack = fact['forward_attack_intent_frames'].get(row['slot'], [])
            row['forward_attack_frame'] = next((f for f in attack if f >= row['commitment_frame']), None)
    finally:
        connection.close()
    report = dict(schema='protodd-ladder-allin-mining-v1', complete=True, **provenance,
        analysis_source_sha256=sha256(Path(__file__)), parser_sha256=sha256(parser),
        audit_sha256=sha256(args.audit), elapsed_seconds=time.monotonic() - started,
        candidates=len(rows), families=summarize(rows), strength_validated=False,
        known_outcomes_all_wins=any(row['won'] is True for row in rows) and
                                all(row['won'] is not False for row in rows),
        limitations=['Descriptive build commitments; not all offensive intent is a confirmed army attack.',
                     'Extracted qualified perspectives may select only winners; all-win cohorts cannot rank strength.',
                     'Source claims and quit inferences are not authoritative match results.',
                     'Observational win rates are confounded by player, opponent, map and survivorship.',
                     'Final test and validation were not used for selecting builds.'])
    (output / 'profiles.jsonl').write_text(''.join(json.dumps(row) + '\n' for row in rows))
    (output / 'replay-facts.json').write_text(json.dumps(facts, indent=2) + '\n')
    (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(dict(complete=True, candidates=len(rows), families=[{k: r[k] for k in
        ('matchup', 'family', 'perspectives', 'known_outcomes', 'wins', 'wilson_95', 'median_ready_frame')}
        for r in report['families']]), indent=2))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tensors', type=Path, required=True)
    parser.add_argument('--audit', type=Path, required=True)
    parser.add_argument('--replays', type=Path, required=True)
    parser.add_argument('--parser', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workers', type=int, default=2)
    arguments = parser.parse_args()
    if not 1 <= arguments.workers <= 4:
        parser.error('use one to four replay parser workers')
    mine(arguments)
