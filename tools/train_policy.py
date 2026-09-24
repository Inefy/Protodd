"""Train sequential race policies only from explicitly reviewed, valid episodes.

PolicyTrace.log contains decisions, never self-certified rewards. This importer
checks both tournament reports, time limits and trace boundaries before assigning
a terminal reward. Frozen evaluation traces are never accepted for training.
"""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from train_openings import require_training_campaign, validate_pair


def transitions(text, own, expected_race, credit='q'):
    if credit not in ('q', 'episode-return'):
        raise ValueError('unknown credit assignment')
    lines = [line.split(',') for line in text.splitlines() if line]
    begins = [r for r in lines if r[0] == 'BEGIN']
    ends = [r for r in lines if r[0] == 'END']
    if len(begins) != 1 or len(ends) != 1:
        raise ValueError('requires one complete policy episode')
    begin = begins[0]
    if len(begin) != 7 or begin[1] != '1' or begin[2] != expected_race or begin[6] != 'train':
        raise ValueError('race/schema mismatch or frozen evaluation episode')
    context = begin[4]
    if context != f'{begin[2]}_{begin[3]}_v1' or not all(c.isalnum() or c in '_-' for c in context):
        raise ValueError('unsafe policy context')
    if len(ends[0]) != 3 or int(ends[0][2]) != int(own['won']) or abs(int(ends[0][1])-own['finalFrame']) > 120:
        raise ValueError('terminal result mismatch')
    decisions = []
    for row in lines:
        if row[0] != 'DECISION':
            continue
        if len(row) != 5:
            raise ValueError('malformed decision')
        frame, state, action, mask = map(int, row[1:])
        if (not 0 <= state < 256 or not 0 <= action < 4 or not 0 < mask < 16 or
                not mask & (1 << action) or frame < 0 or frame > int(ends[0][1]) or
                (decisions and frame <= decisions[-1][0])):
            raise ValueError('invalid or non-monotonic policy decision')
        decisions.append((frame, state, action, mask))
    if not decisions:
        raise ValueError('no decisions')
    records = []
    visited = set()
    for i, (_, state, action, _) in enumerate(decisions):
        if credit == 'episode-return':
            # First-visit Monte Carlo: observed episode outcome, no imagined
            # untried-action bootstrap. One update per state/action per game
            # avoids weighting long stalls more heavily. Undiscounted terminal
            # reward avoids making longer survival a less-negative loss.
            if (state, action) in visited:
                continue
            visited.add((state, action))
            reward = 1 if own['won'] else -1
            records.append(f'{context} {state} {action} {reward} 0 0 1')
            continue
        terminal = i == len(decisions)-1
        next_state = 0 if terminal else decisions[i+1][1]
        next_mask = 0 if terminal else decisions[i+1][3]
        reward = (1 if own['won'] else -1) if terminal else 0
        records.append(f'{context} {state} {action} {reward} {next_state} {next_mask} {int(terminal)}')
    # Backward replay propagates terminal feedback through this episode without
    # pretending intermediate mineral/survival changes are strategic wins.
    return list(reversed(records))


def read_episode(run, gid, credit='q'):
    require_training_campaign(run)
    manifest = json.loads((run/'manifest.json').read_text())
    bot, race = manifest['bot'], manifest['race']
    settings = json.loads((run/'server/server_settings.json').read_text())
    rows = [json.loads(r) for r in (run/'server/results.jsonl').read_text().splitlines()]
    own = validate_pair([r for r in rows if r['gameID'] == gid],
                        settings['tournamentModuleSettings']['timeoutLimits'], bot)
    paths = list((run/f'server/replays/bot-write/game-{gid}/{bot}').glob('*/PolicyTrace.log'))
    if len(paths) != 1:
        raise ValueError('missing or ambiguous policy trace')
    data = paths[0].read_bytes()
    records = transitions(data.decode('utf-8'), own, race, credit)
    return records, dict(run=str(run.resolve()), bot=bot, race=race, gameID=gid,
        sha256=hashlib.sha256(data).hexdigest(), reward=1 if own['won'] else -1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--game-ids', type=int, nargs='+', required=True)
    parser.add_argument('--trainer', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--previous-policy', type=Path)
    parser.add_argument('--credit-assignment', choices=['q', 'episode-return'], default='q')
    args = parser.parse_args()
    if args.output.exists():
        parser.error('refusing to overwrite training snapshot')
    if len(set(args.game_ids)) != len(args.game_ids):
        parser.error('duplicate episodes')
    manifest = json.loads((args.run/'manifest.json').read_text())
    bot, race = manifest['bot'], manifest['race']
    records, sources = [], []
    selected = {}
    if args.previous_policy:
        previous = json.loads((args.previous_policy/'manifest.json').read_text())
        for old in previous['reviewedEpisodes']:
            run = Path(old['run'])
            episode, source = read_episode(run, old['gameID'], args.credit_assignment)
            if source != old:
                raise ValueError('previously reviewed policy evidence changed')
            selected[(str(run.resolve()).casefold(), old['gameID'])] = (episode, source)
    for gid in args.game_ids:
        selected[(str(args.run.resolve()).casefold(), gid)] = read_episode(args.run, gid, args.credit_assignment)
    for episode, source in selected.values():
        records.extend(episode)
        sources.append(source)
    args.output.mkdir(parents=True)
    dataset = args.output/'transitions.txt'
    dataset.write_text('\n'.join(records)+'\n', encoding='utf-8')
    # Trainer produces a fresh table. To avoid accidental double learning,
    # rebuild with a complete reviewed dataset, never replay into an old table.
    subprocess.run([str(args.trainer.resolve()), str(dataset.resolve()),
                    str((args.output/'Policy.q').resolve())], check=True)
    (args.output/'manifest.json').write_text(json.dumps(dict(
        run=str(args.run.resolve()), bot=bot, race=race, reviewedEpisodes=sources,
        transitionCount=len(records), heldOut=False,
        creditAssignment=args.credit_assignment,
        reward='terminal strategic win +1/loss -1; no score or survival bonus'), indent=2))


if __name__ == '__main__':
    main()
