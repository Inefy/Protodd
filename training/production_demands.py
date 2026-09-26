"""Concurrent production targets from complete, verified accepted-action logs.

Future events are targets only. Features add strictly past own-command history
to the existing legal macro encoding. Command acceptance is not completion.
"""
import gzip
import json
from pathlib import Path

import numpy as np

from .schema import load_schema, sha256

ACTIONS = ('train_probe', 'build_pylon', 'build_gateway', 'build_assimilator',
           'build_cybernetics_core', 'train_zealot', 'train_dragoon', 'expand_nexus')
HORIZON = 240
MAX_COUNT = 4


def demand_rows(rows, events, owner, valid_through, horizon=HORIZON):
    frames = rows['frames'].astype(np.int64)
    if horizon < 24 or (np.diff(frames) <= 0).any():
        raise ValueError('invalid demand chronology')
    own = [e for e in events if e['owner'] == owner and e['accepted'] and not e['repeated']]
    if any(own[i]['frame'] > own[i+1]['frame'] for i in range(len(own)-1)):
        raise ValueError('action log is out of order')
    names = load_schema()['actions']
    # Bind the full log to the previously validated first-action tensor labels.
    event_keys = {(e['frame'], e['action']) for e in own}
    for i in np.flatnonzero(rows['labels']):
        if (int(rows['action_frames'][i]), names[int(rows['labels'][i])]) not in event_keys:
            raise ValueError('tensor label does not match accepted-action log')
    truth = np.zeros((len(frames), len(ACTIONS)), dtype=np.int64)
    previous = np.zeros_like(truth)
    history = np.zeros((len(frames), len(ACTIONS)*2), dtype=np.float32)
    for j, action in enumerate(ACTIONS):
        times = np.array([e['frame'] for e in own if e['action'] == action], dtype=np.int64)
        at = np.searchsorted(times, frames, side='left')
        truth[:, j] = np.searchsorted(times, frames+horizon, side='left') - at
        previous[:, j] = at - np.searchsorted(times, frames-horizon, side='left')
        history[:, j] = np.minimum(previous[:, j], MAX_COUNT) / MAX_COUNT
        age = np.full(len(frames), 2400, dtype=np.int64)
        observed = at > 0
        age[observed] = frames[observed] - times[at[observed]-1]
        history[:, len(ACTIONS)+j] = np.minimum(age, 2400) / 2400
    # Full action logs cover gaps in tensor rows. Only the replay end censors
    # future counts; current intent masks must not erase future prerequisites.
    valid = frames+horizon <= valid_through
    x = np.concatenate((rows['features'], history), axis=1)
    return x, truth, previous, valid


class AcceptedActionSource:
    def __init__(self, tensor_manifest):
        self.manifest_path = Path(tensor_manifest['source_manifest'])
        if sha256(self.manifest_path) != tensor_manifest['source_manifest_sha256']:
            raise ValueError('macro source manifest changed')
        binding = tensor_manifest['extraction_release']
        for name in ('release', 'sources'):
            if sha256(binding[name+'_path']) != binding[name+'_sha256']:
                raise ValueError('macro release binding changed')
        self.sources = json.loads(Path(binding['sources_path']).read_text())['games']
        self.games = {g['game_id']: g for g in json.loads(self.manifest_path.read_text())['games']}
        self.receipts = {}

    def read(self, game_id, allowed_split='train'):
        game, source = self.games[game_id], self.sources[game_id]
        if allowed_split not in ('train', 'validation') or game['split'] != allowed_split:
            raise ValueError('unapproved action-log split; final test remains sealed')
        directory = Path(source['directory'])
        result_path, actions_path = directory/'result.json', directory/'actions.jsonl.gz'
        if sha256(result_path) != source['result_sha256'] or sha256(actions_path) != source['actions_sha256']:
            raise ValueError('action log or extraction receipt changed')
        result = json.loads(result_path.read_text())
        if (result['status'] != 'extracted' or result['game_id'] != game_id
                or result['actions_sha256'] != source['actions_sha256']
                or result['stats']['end_frame'] != game['valid_through_frame']):
            raise ValueError('action extraction receipt mismatch')
        with gzip.open(actions_path, 'rt', encoding='utf-8') as stream:
            events = [json.loads(line) for line in stream]
        self.receipts[game_id] = dict(actions_sha256=source['actions_sha256'],
            result_sha256=source['result_sha256'], duplicate_group=game['duplicate_group'],
            replay_sha256=game['replay_sha256'])
        return game, events


def collect(dataset, ids, *, max_frame=7200, progress=None, split='train', perspectives=None):
    if split not in ('train','validation'):
        raise ValueError('final test is sealed')
    source = AcceptedActionSource(dataset.manifest)
    chunks = {k: [] for k in ('features', 'counts', 'previous', 'games', 'frames', 'perspectives')}
    details, cached_game, events = {}, None, None
    for sequence in dataset.sequences:
        game_id = sequence['game_id']
        if game_id not in ids:
            continue
        if perspectives is not None and sequence['perspective']!=perspectives[game_id]:
            continue
        if sequence['split'] != split:
            raise ValueError('demand cohort split differs')
        if game_id != cached_game:
            game, events = source.read(game_id,split)
            cached_game = game_id
        perspective = sequence['perspective']
        if game['player_quality'][perspective] == 'unknown':
            raise ValueError('unqualified training perspective')
        rows = dataset.read_rows(sequence['start'], sequence['count'])
        x, counts, previous, valid = demand_rows(rows, events, game['slots'][perspective], game['valid_through_frame'])
        valid &= rows['frames'] < max_frame
        for name, values in dict(features=x, counts=counts, previous=previous,
                games=rows['game_indices'].astype(np.int64), frames=rows['frames'],
                perspectives=rows['perspectives']).items():
            chunks[name].append(values[valid])
        detail = details.setdefault(game_id, dict(rows=0, positives=0, first_label_events=0,
            all_accepted_events=0, concurrent_types=0))
        detail['rows'] += int(valid.sum())
        detail['positives'] += int(counts[valid].sum())
        detail['concurrent_types'] += int(((counts[valid]>0).sum(1)>1).sum())
        owner = game['slots'][perspective]
        action_ids = {load_schema()['actions'].index(name) for name in ACTIONS}
        detail['first_label_events'] += int((np.isin(rows['labels'], list(action_ids)) & (rows['frames']<max_frame)).sum())
        detail['all_accepted_events'] += sum(e['owner']==owner and e['accepted'] and not e['repeated']
            and e['action'] in ACTIONS and 0<=e['frame']<max_frame for e in events)
        if progress:
            progress(len(details), len(ids))
    if set(details) != set(ids) or not all(d['rows'] for d in details.values()):
        raise ValueError('missing demand cohort game')
    return {k:np.concatenate(v) for k,v in chunks.items()}, details, source.receipts
