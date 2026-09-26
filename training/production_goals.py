"""Causal inputs and longer-horizon population targets; offline train data only."""
import numpy as np

from .production_demands import ACTIONS, AcceptedActionSource, demand_rows
from .schema import load_schema

UNITS = ('probe', 'zealot', 'dragoon')
ACTION_INDICES = (0, 5, 6)
HORIZON = 1200


def population(features, prefix='own_complete/'):
    schema = load_schema()['features']
    lookup = {f['name']: (i, f['scale']) for i, f in enumerate(schema)}
    return np.stack([np.rint(features[:, lookup[prefix+unit][0]] * lookup[prefix+unit][1])
                     for unit in UNITS], axis=1).astype(np.int64)


def goal_rows(rows, events, owner, valid_through, horizon=HORIZON):
    """Peak completed population in [t,t+horizon], with fully observed cadence.

    The legal encoder counts waiting queue entries separately from the active
    incomplete unit. Current committed stock is therefore complete+incomplete+
    waiting, not a max or a double count of active training.
    """
    frames = rows['frames'].astype(np.int64)
    if not len(frames) or horizon < 24 or (np.diff(frames) <= 0).any():
        raise ValueError('invalid population chronology')
    x, _, _, _ = demand_rows(rows, events, owner, valid_through)
    complete = population(x)
    committed = complete + population(x, 'own_incomplete/') + population(x, 'own_queue/')
    goals = np.zeros_like(complete)
    valid = np.zeros(len(frames), dtype=bool)
    priority = np.full(len(frames), -1, dtype=np.int64)
    own = [e for e in events if e['owner'] == owner and e['accepted'] and not e['repeated']]
    times = [np.array([e['frame'] for e in own if e['action'] == ACTIONS[j]], dtype=np.int64)
             for j in ACTION_INDICES]
    for i, frame in enumerate(frames):
        end = int(np.searchsorted(frames, frame+horizon))
        # Exact endpoint plus continuous 24-frame observations; never bridge
        # a shard sampling gap or quietly convert a censored future to zero.
        if (end >= len(frames) or frames[end] != frame+horizon
                or frame+horizon > valid_through or (np.diff(frames[i:end+1]) > 24).any()):
            continue
        valid[i] = True
        goals[i] = complete[i:end+1].max(axis=0)
        first = np.full(3, np.iinfo(np.int64).max, dtype=np.int64)
        for j, events_at in enumerate(times):
            at = np.searchsorted(events_at, frame)
            if (goals[i, j] > committed[i, j] and at < len(events_at)
                    and events_at[at] < frame+horizon):
                first[j] = events_at[at]
        if first.min() < np.iinfo(np.int64).max:
            # Same-frame ties are ambiguous priority labels, not arbitrary IDs.
            if (first == first.min()).sum() == 1:
                priority[i] = int(first.argmin())
    return dict(features=x, goals=goals, committed=committed, priority=priority), valid


def collect(dataset, ids, progress=None):
    source = AcceptedActionSource(dataset.manifest)
    chunks = {k: [] for k in ('features', 'goals', 'committed', 'priority', 'games', 'frames', 'perspectives')}
    details = {}
    cached_id, events = None, None
    for sequence in dataset.sequences:
        game_id = sequence['game_id']
        if game_id not in ids:
            continue
        if sequence['split'] != 'train':
            raise ValueError('goal development must use train games only')
        if game_id != cached_id:
            game, events = source.read(game_id)
            cached_id = game_id
        perspective = sequence['perspective']
        if game['player_quality'][perspective] == 'unknown':
            raise ValueError('unqualified goal perspective')
        rows = dataset.read_rows(sequence['start'], sequence['count'])
        # Only read the necessary early trajectory into target construction.
        keep = rows['frames'] <= 7200+HORIZON
        rows = {key: value[keep] for key, value in rows.items()}
        data, valid = goal_rows(rows, events, game['slots'][perspective], game['valid_through_frame'])
        valid &= rows['frames'] < 7200
        data.update(games=rows['game_indices'], frames=rows['frames'], perspectives=rows['perspectives'])
        for key in chunks:
            chunks[key].append(data[key][valid])
        details[game_id] = details.get(game_id, 0) + int(valid.sum())
        if progress:
            progress(len(details), len(ids))
    if set(details) != set(ids) or not all(details.values()):
        raise ValueError('missing goal cohort')
    return {k: np.concatenate(v) for k, v in chunks.items()}, details, source.receipts


class GoalLedger:
    """Diagnostic absolute-goal execution contract, independent of any fit.

    Repeating the same observation cannot replenish accepted production. A new
    measured stock snapshot replaces that local accounting; live integration
    would additionally reconcile pending acceptance with the native ledger.
    """
    def __init__(self):
        self.frame = -1
        self.goals = np.zeros(3, dtype=np.int64)
        self.stock = np.zeros(3, dtype=np.int64)

    def observe(self, frame, goals, committed):
        if frame <= self.frame:
            return False
        if frame < 0 or len(goals) != 3 or len(committed) != 3 or min(*goals, *committed) < 0:
            raise ValueError('invalid goal snapshot')
        self.frame = frame
        self.goals = np.array(goals, dtype=np.int64)
        self.stock = np.array(committed, dtype=np.int64)
        return True

    def deficits(self, frame):
        if frame < self.frame or frame >= self.frame+HORIZON or self.frame < 0:
            return np.zeros(3, dtype=np.int64)
        return np.maximum(self.goals-self.stock, 0)

    def accept(self, frame, action):
        if action not in range(3) or self.deficits(frame)[action] <= 0:
            raise ValueError('acceptance outside population deficit')
        self.stock[action] += 1
