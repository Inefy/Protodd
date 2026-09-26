"""Unchanged worker-goal weights and gates on the reserved validation cohort."""
import argparse
import json
import os
from pathlib import Path
import time

import numpy as np

from .macro_commitment_probe import write
from .production_demands import AcceptedActionSource
from .production_goals import goal_rows, HORIZON
from .production_worker_goal import metrics, gates, GATES
from .schema import sha256
from .shards import TensorShards


def run(args):
    if args.output.exists():
        raise FileExistsError(args.output)
    previous = json.loads((args.candidate/'report.json').read_text())
    prior_spec = json.loads((args.candidate/'run.json').read_text())
    if not previous['holdout']['passed'] or prior_spec['gates'] != GATES:
        raise ValueError('worker holdout must pass; thresholds must be unchanged')
    model_path = args.candidate/'worker-goal.npz'
    if sha256(model_path) != previous['model_sha256']:
        raise ValueError('worker weights changed')
    for name in ('production_worker_goal.py', 'production_goals.py'):
        if sha256(Path(__file__).with_name(name)) != prior_spec['source_sha256'][name]:
            raise ValueError('worker targets/metrics changed')
    inventory = json.loads(args.inventory.read_text())
    chosen = inventory['selected']
    selected = {row['game_id']: row for row in chosen}
    if len(selected) != 24 or any(row['split'] != 'validation' for row in chosen):
        raise ValueError('reserved validation cohort required')
    if sha256(args.dataset/'manifest.json') != prior_spec['dataset_manifest_sha256']:
        raise ValueError('validation release changed')
    fit = Path(prior_spec['fit'])
    if sha256(fit/'run.json') != prior_spec['fit_run_sha256'] or sha256(fit/'data.json') != prior_spec['fit_data_sha256']:
        raise ValueError('original fit evidence changed')
    fit_spec = json.loads((fit/'run.json').read_text())
    used_ids = set(fit_spec['train_games']+fit_spec['dev_games']) | {g for values in prior_spec['selected'].values() for g in values}
    if used_ids & set(selected):
        raise ValueError('validation overlaps development')
    args.output.mkdir(parents=True)
    def status(stage, **values):
        write(args.output/'status.json', dict(stage=stage, pid=os.getpid(), updated_unix=time.time(), **values))
    write(args.output/'run.json', dict(schema='protodd-worker-goal-confirmation-v1',
          model_sha256=sha256(model_path), candidate_report_sha256=sha256(args.candidate/'report.json'),
          candidate_run_sha256=sha256(args.candidate/'run.json'), inventory_sha256=sha256(args.inventory),
          selected=chosen, gates=GATES, additional_optimizer_updates=0,
          source_sha256={p.name: sha256(p) for p in Path(__file__).parent.glob('*.py')},
          globally_fresh=False, prior_macro_and_production_validation_exposure=True,
          final_test_opened=False, live_control_allowed=False))
    status('verifying_tensors')
    chunks = {k: [] for k in ('features', 'goals', 'committed', 'priority', 'games', 'frames', 'perspectives')}
    details = {}
    matchup_by_index = {}
    with TensorShards(args.dataset, progress=lambda n, total: status('verifying_tensors', rows=n, total=total)) as dataset:
        source = AcceptedActionSource(dataset.manifest)
        for sequence in dataset.sequences:
            game_id = sequence['game_id']
            if game_id not in selected or sequence['perspective'] != selected[game_id]['perspective']:
                continue
            if sequence['split'] != 'validation':
                raise ValueError('wrong confirmation split')
            game, events = source.read(game_id, 'validation')
            if source.receipts[game_id]['replay_sha256'] != selected[game_id]['replay_sha256']:
                raise ValueError('reserved replay identity changed')
            p = sequence['perspective']
            if game['player_quality'][p] == 'unknown':
                raise ValueError('unqualified confirmation perspective')
            rows = dataset.read_rows(sequence['start'], sequence['count'])
            keep = rows['frames'] <= 7200+HORIZON
            rows = {k: v[keep] for k, v in rows.items()}
            data, valid = goal_rows(rows, events, game['slots'][p], game['valid_through_frame'])
            valid &= rows['frames'] < 7200
            data.update(games=rows['game_indices'], frames=rows['frames'], perspectives=rows['perspectives'])
            for key in chunks:
                chunks[key].append(data[key][valid])
            details[game_id] = int(valid.sum())
            matchup_by_index[sequence['game_index']] = sequence['matchup']
            status('collecting_confirmation', games=len(details), total=24)
    if set(details) != set(selected) or not all(details.values()):
        raise ValueError('missing confirmation game')
    old_selection = json.loads((fit/'selection.json').read_text())
    check_selection = json.loads((args.candidate/'selection.json').read_text())
    prior_receipts = [old_selection[s]['receipts'] for s in ('train', 'dev')] + [check_selection['receipts']]
    for field in ('duplicate_group', 'replay_sha256'):
        if {v[field] for receipt in prior_receipts for v in receipt.values()} & {v[field] for v in source.receipts.values()}:
            raise ValueError('confirmation replay alias')
    write(args.output/'selection.json', dict(rows_by_game=details, receipts=source.receipts))
    data = {k: np.concatenate(v) for k, v in chunks.items()}
    np.savez_compressed(args.output/'confirmation.npz', **data)
    write(args.output/'data.json', dict(confirmation=sha256(args.output/'confirmation.npz')))
    with np.load(model_path, allow_pickle=False) as model:
        prediction = ((data['features']-model['mean'])/model['scale'])@model['weight'][:-1]+model['weight'][-1]
    digest = json.loads((fit/'data.json').read_text())['train']
    if sha256(fit/'train.npz') != digest:
        raise ValueError('reference training cache changed')
    with np.load(fit/'train.npz', allow_pickle=False) as archive:
        train = dict(archive)
    def bins(d):
        return (d['frames']//1200)*3+d['features'][:, 9:12].argmax(1)
    tb, db = bins(train), bins(data)
    clock = np.full(len(data['goals']), np.median(train['goals'][:, 0]))
    for key in np.unique(tb):
        clock[db == key] = np.median(train['goals'][tb == key, 0])
    reference = dict(persistence=metrics(data['committed'][:, 0], data), time_race_median=metrics(clock, data))
    candidate = metrics(prediction, data)
    by_matchup = {}
    for matchup in ('PvP', 'PvT', 'PvZ'):
        mask = np.array([matchup_by_index[int(g)] == matchup for g in data['games']])
        by_matchup[matchup] = metrics(prediction[mask], {k: v[mask] for k, v in data.items()})
    gate = gates(candidate, reference)
    report = dict(candidate=candidate, by_matchup=by_matchup, reference=reference, confirmation=gate,
                  model_sha256=sha256(model_path), unchanged_parameters=True, additional_optimizer_updates=0,
                  globally_fresh=False, strength_proven=False, final_test_opened=False,
                  live_control_allowed=False, promotion_eligible=False)
    write(args.output/'report.json', report)
    status('complete', confirmation_pass=gate['passed'])
    print(json.dumps(dict(confirmation=gate, candidate=candidate)), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dataset', 'candidate', 'inventory', 'output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    try:
        run(args)
    except Exception as error:
        path = args.output/'status.json'
        if path.exists():
            state = json.loads(path.read_text())
            if state.get('pid') == os.getpid():
                state.update(stage='failed', error=repr(error), updated_unix=time.time())
                write(path, state)
        raise
