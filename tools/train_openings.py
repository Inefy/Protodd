"""Build a new, auditable opening-bandit snapshot from reviewed paired games.

Explicit game IDs are an activity-review allowlist, NOT automatic health proof.
Rebuild from all intended training runs; never include held-out evaluation games.
No files in a running tournament are modified.
"""
import argparse
import hashlib
import json
from collections import Counter, defaultdict
from pathlib import Path

STYLES = {'standard', 'aggressive', 'economic', 'deceptive'}


def require_training_campaign(run):
    """Declared evaluation purpose wins even if a trace accidentally says train."""
    path = Path(run) / 'manifest.json'
    if path.exists():
        manifest = json.loads(path.read_text())
        if ('purpose' in manifest or manifest.get('format') == 'protodd-arena-v1') and manifest.get('purpose') != 'training':
            raise ValueError('evaluation or unknown campaign purpose cannot train')


def validate_pair(rows, timeout_limits=(), bot='Protodd'):
    if len(rows) != 2 or len({r['reportingBot'] for r in rows}) != 2:
        raise ValueError('requires exactly two distinct reports')
    own = next((r for r in rows if r['reportingBot'] == bot), None)
    if own is None:
        raise ValueError('no selected bot report')
    enemy = next(r for r in rows if r is not own)
    if any(r.get('gameEndType') != 'NORMAL' or r.get('crash') is not False or
           r.get('gameTimeout') is not False for r in rows):
        raise ValueError('abnormal result')
    for row in rows:
        timers = {t['timeInMS']: t['frameCount'] for t in row.get('timers', [])}
        for limit in timeout_limits:
            if limit['timeInMS'] not in timers:
                raise ValueError('missing runtime-limit telemetry')
            if timers[limit['timeInMS']] >= limit['frameCount']:
                raise ValueError('runtime limit reached; not a strategic outcome')
    if (own['opponentBot'] != enemy['reportingBot'] or
            enemy['opponentBot'] != bot or own['map'] != enemy['map'] or
            type(own['won']) is not bool or type(enemy['won']) is not bool or
            own['won'] == enemy['won'] or
            abs(own['finalFrame'] - enemy['finalFrame']) > 120):
        raise ValueError('inconsistent pair')
    return own


def read_episode(run, game_id):
    require_training_campaign(run)
    rows = [json.loads(line) for line in (run / 'server/results.jsonl').read_text().splitlines()]
    settings = json.loads((run / 'server/server_settings.json').read_text())
    own = validate_pair([r for r in rows if r['gameID'] == game_id],
                        settings['tournamentModuleSettings']['timeoutLimits'])
    logs = list((run / f'server/replays/bot-write/game-{game_id}/Protodd').glob('*/Protodd.log'))
    if len(logs) != 1:
        raise ValueError('requires one unambiguous archived trace')
    data = logs[0].read_bytes()
    lines = data.decode('utf-8', errors='replace').splitlines()
    modes = [line for line in lines if line.startswith('LEARNING,')]
    if modes != ['LEARNING,mode=validated-train']:
        raise ValueError('frozen or unknown opening-learning episode')
    starts = [line.split(',') for line in lines if line.startswith('START,')]
    ends = [line.split(',') for line in lines if line.startswith('END,')]
    if len(starts) != 1 or len(ends) != 1:
        raise ValueError('missing or ambiguous episode boundaries')
    _, map_name, opponent, style = starts[0]
    if (opponent != own['opponentBot'] or style not in STYLES or
            ends[0][1] != ('win' if own['won'] else 'loss') or
            abs(int(ends[0][2]) - own['finalFrame']) > 120):
        raise ValueError('trace/result mismatch')
    return (opponent, map_name, style), own['won'], {
        'run': str(run.resolve()), 'gameID': game_id,
        'traceSHA256': hashlib.sha256(data).hexdigest(),
        'resultsSHA256': hashlib.sha256((run / 'server/results.jsonl').read_bytes()).hexdigest(),
        'reward': int(own['won']), 'style': style,
    }


def merge_episodes(episodes):
    """Count each reviewed episode once, not cumulative snapshot counters."""
    unique = {}
    for key, won, source in episodes:
        identity = (str(Path(source['run']).resolve()).casefold(), source['gameID'])
        previous = unique.get(identity)
        if previous is not None:
            old_key, old_won, old_source = previous
            if (old_key != key or old_won != won or
                    old_source['traceSHA256'] != source['traceSHA256']):
                raise ValueError('conflicting evidence for the same episode')
            continue
        unique[identity] = (key, won, source)
    return list(unique.values())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--game-ids', type=int, nargs='+', required=True,
                        help='Only explicitly reviewed, functioning-opponent games')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--previous-policy', type=Path,
                        help='Revalidate and retain this snapshot\'s reviewed episodes')
    parser.add_argument('--mode', choices=['validated-train', 'frozen'], default='validated-train')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refusing to overwrite a policy snapshot')
    if len(set(args.game_ids)) != len(args.game_ids):
        parser.error('duplicate game IDs')
    counts = defaultdict(Counter)
    provenance = []
    episodes = []
    if args.previous_policy:
        previous = json.loads((args.previous_policy / 'training-manifest.json').read_text())
        for source in previous['episodes']:
            episode = read_episode(Path(source['run']), source['gameID'])
            if any(episode[2][field] != source[field]
                   for field in ('traceSHA256', 'reward', 'style')):
                raise ValueError('previously reviewed evidence has changed')
            episodes.append(episode)
    for game_id in args.game_ids:
        episodes.append(read_episode(args.run, game_id))
    for key, won, source in merge_episodes(episodes):
        counts[key]['wins' if won else 'losses'] += 1
        provenance.append(source)
    files = defaultdict(list)
    for (opponent, map_name, style), result in sorted(counts.items()):
        clean = lambda value: ''.join('_' if c in ',\n\r#' else c for c in value)
        files[opponent].append(f'{clean(opponent)},{clean(map_name)},{style},{result["wins"]},{result["losses"]}\n')
    args.output.mkdir(parents=True)
    for opponent, lines in files.items():
        (args.output / f'Protodd-{opponent.encode().hex()}.csv').write_text(''.join(lines))
    (args.output / 'Protodd-learning-mode.txt').write_text(args.mode + '\n')
    (args.output / 'training-manifest.json').write_text(json.dumps({
        'algorithm': 'opponent/map UCB with capped cross-map prior',
        'reward': 'terminal win=1, loss=0; no score/survival shaping',
        'activityReview': 'explicit caller-approved game IDs; paired NORMAL is not health proof',
        'episodes': provenance, 'mode': args.mode,
    }, indent=2))
    print(f'Wrote {len(provenance)} reviewed episodes to {args.output}')


if __name__ == '__main__':
    main()
