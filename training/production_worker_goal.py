"""Freeze a learned worker-population policy, then check a disjoint train cohort.

This is the second bounded population-goal variant. Army ordering from the
failed joint model is not used. The linear policy is selected using development
evidence; its parameters and criteria are fixed before reading held-out rows.
"""
import argparse
import json
import os
from pathlib import Path
import time

import numpy as np
import torch

from .macro_commitment_probe import write
from .production_goals import collect
from .schema import sha256
from .shards import TensorShards

GATES = dict(mae_max=1.25, persistence_mae_ratio=.75, clock_mae_ratio=.90,
             growth_precision=.90, growth_recall=.90,
             late_growth_precision=.85, late_growth_recall=.85,
             per_game_growth_recall=.70, required_late_positives=30)


class WorkerGoalLedger:
    """Persistent absolute goals: lower forecasts cannot erase unexpired goals.

    Pure contract model, not the live acceptance/reconciliation implementation.
    Each proposal expires at its original horizon even when later goals arrive.
    """
    def __init__(self):
        self.last = -1
        self.proposals = []

    def propose(self, frame, goal):
        if frame <= self.last:
            return False
        if frame < 0 or not np.isfinite(goal) or goal < 0 or goal > 80:
            raise ValueError('invalid worker goal')
        self.last = frame
        self.proposals = [(end, g) for end, g in self.proposals if end > frame]
        self.proposals.append((frame+1200, int(np.rint(goal))))
        return True

    def deficit(self, frame, committed):
        if frame < self.last or committed < 0:
            raise ValueError('invalid stock observation')
        goal = max((g for end, g in self.proposals if end > frame), default=0)
        return max(0, goal-committed)


def metrics(predicted, data):
    pred = np.maximum(np.rint(predicted), 0).astype(np.int64)
    truth, stock = data['goals'][:, 0], data['committed'][:, 0]
    def growth(mask):
        p, t = pred[mask] > stock[mask], truth[mask] > stock[mask]
        tp = int((p&t).sum())
        return dict(positive_rows=int(t.sum()), predicted_rows=int(p.sum()),
                    precision=tp/max(1, int(p.sum())), recall=tp/max(1, int(t.sum())))
    return dict(rows=len(pred), mae=float(np.abs(pred-truth).mean()),
                growth=growth(np.ones(len(pred), dtype=bool)),
                late=growth(data['frames'] >= 4800),
                by_phase={str(f): growth((data['frames'] >= f) & (data['frames'] < f+1200)) for f in range(0, 7200, 1200)},
                per_game={str(g): growth(data['games'] == g) for g in np.unique(data['games'])},
                excessive_goals=int((pred > 80).sum()))


def gates(candidate, reference):
    checks = dict(mae=candidate['mae'] <= GATES['mae_max'],
                  beats_persistence=candidate['mae'] <= reference['persistence']['mae']*GATES['persistence_mae_ratio'],
                  beats_clock=candidate['mae'] <= reference['time_race_median']['mae']*GATES['clock_mae_ratio'],
                  growth_precision=candidate['growth']['precision'] >= GATES['growth_precision'],
                  growth_recall=candidate['growth']['recall'] >= GATES['growth_recall'],
                  late_support=candidate['late']['positive_rows'] >= GATES['required_late_positives'],
                  late_precision=candidate['late']['precision'] >= GATES['late_growth_precision'],
                  late_recall=candidate['late']['recall'] >= GATES['late_growth_recall'],
                  per_game_recall=all(g['positive_rows'] > 0 and g['recall'] >= GATES['per_game_growth_recall'] for g in candidate['per_game'].values()),
                  bounded_goals=candidate['excessive_goals'] == 0)
    return dict(passed=all(checks.values()), checks=checks)


def run(args):
    if args.output.exists():
        raise FileExistsError(args.output)
    args.output.mkdir(parents=True)
    def status(stage, **values):
        write(args.output/'status.json', dict(stage=stage, pid=os.getpid(), updated_unix=time.time(), **values))
    status('verifying_training_cache')
    torch.set_num_threads(2)
    fit_spec = json.loads((args.fit/'run.json').read_text())
    manifest_path = args.dataset/'manifest.json'
    if sha256(manifest_path) != fit_spec['dataset_manifest_sha256']:
        raise ValueError('tensor release changed')
    if sha256(Path(__file__).with_name('production_goals.py')) != fit_spec['source_sha256']['production_goals.py']:
        raise ValueError('target construction changed')
    cache = json.loads((args.fit/'data.json').read_text())
    for name in ('train', 'dev'):
        if sha256(args.fit/(name+'.npz')) != cache[name]:
            raise ValueError('fitted data changed')
    with np.load(args.fit/'train.npz', allow_pickle=False) as values:
        train = dict(values)
    with np.load(args.fit/'dev.npz', allow_pickle=False) as values:
        dev = dict(values)
    cohort = json.loads(args.cohort.read_text())
    selected = cohort['selected']
    ids = {g for values in selected.values() for g in values}
    full_path = Path(cohort['excluded_full_fit_run'])
    if sha256(full_path) != cohort['excluded_full_fit_sha256']:
        raise ValueError('full-fit exclusion changed')
    used = {g for group in json.loads(full_path.read_text())['groups'] for values in group.values() for g in values}
    if len(ids) != 9 or ids & (used | set(fit_spec['train_games']+fit_spec['dev_games'])):
        raise ValueError('worker holdout overlap or size mismatch')
    spec = dict(schema='protodd-worker-goal-check-v1', controlled_scope='train_probe only',
                fit=str(args.fit.resolve()), fit_run_sha256=sha256(args.fit/'run.json'),
                fit_data_sha256=sha256(args.fit/'data.json'), cohort_sha256=sha256(args.cohort),
                dataset_manifest_sha256=sha256(manifest_path), gates=GATES,
                source_sha256={p.name: sha256(p) for p in Path(__file__).parent.glob('*.py')},
                selected=selected, globally_fresh=False, split='train',
                selection='metadata-only disjoint games, omitted from whole-game full fit',
                training_games=36, training_rows=len(train['goals']), ridge_regularization=.01,
                target='peak completed Probes over next 1200 frames',
                execution='maximum unexpired absolute goal, each original expiry t+1200; subtract actual complete/incomplete/waiting/pending once',
                final_test_opened=False, evaluation_game_traces_used=False, live_control_allowed=False)
    write(args.output/'run.json', spec)
    status('fitting_linear_policy')
    x = torch.from_numpy(train['features']).double()
    mean, scale = x.mean(0), x.std(0).clamp(min=.02)
    x = torch.cat(((x-mean)/scale, torch.ones((len(x), 1), dtype=x.dtype)), 1)
    y = torch.from_numpy(train['goals'][:, :1]).double()
    regularizer = torch.eye(x.shape[1], dtype=x.dtype)*len(x)*.01
    regularizer[-1, -1] = 0
    weight = torch.linalg.solve(x.T@x+regularizer, x.T@y).numpy().ravel()
    mean, scale = mean.numpy(), scale.numpy()
    np.savez(args.output/'worker-goal.npz', mean=mean, scale=scale, weight=weight)
    def predict(data):
        return ((data['features']-mean)/scale)@weight[:-1]+weight[-1]
    # Record that this is the development-selected baseline, not a silently
    # changed neural checkpoint. Freeze its bytes before opening holdout rows.
    write(args.output/'frozen-model.json', dict(model_sha256=sha256(args.output/'worker-goal.npz'),
          development=metrics(predict(dev), dev), parameters_frozen_before_holdout=True))
    status('verifying_holdout_tensors')
    with TensorShards(args.dataset, progress=lambda n, total: status('verifying_holdout_tensors', rows=n, total=total)) as dataset:
        data, detail, receipts = collect(dataset, ids, progress=lambda n, total: status('collecting_holdout', games=n, total=total))
    prior_selection = json.loads((args.fit/'selection.json').read_text())
    for field in ('duplicate_group', 'replay_sha256'):
        prior = {v[field] for split in ('train', 'dev') for v in prior_selection[split]['receipts'].values()}
        if prior & {v[field] for v in receipts.values()}:
            raise ValueError('worker holdout replay alias')
    write(args.output/'selection.json', dict(rows_by_game=detail, receipts=receipts))
    np.savez_compressed(args.output/'holdout.npz', **data)
    write(args.output/'data.json', dict(holdout=sha256(args.output/'holdout.npz')))
    status('evaluating')
    def bins(d):
        return (d['frames']//1200)*3+d['features'][:, 9:12].argmax(1)
    tb, db = bins(train), bins(data)
    clock = np.full(len(data['goals']), np.median(train['goals'][:, 0]))
    for key in np.unique(tb):
        clock[db == key] = np.median(train['goals'][tb == key, 0])
    reference = dict(persistence=metrics(data['committed'][:, 0], data), time_race_median=metrics(clock, data))
    candidate = metrics(predict(data), data)
    gate = gates(candidate, reference)
    report = dict(schema='protodd-worker-goal-result-v1', candidate=candidate, reference=reference,
                  holdout=gate, model_sha256=sha256(args.output/'worker-goal.npz'),
                  selected_using_development=True, gates_frozen_before_holdout=True,
                  strength_proven=False, live_control_allowed=False, promotion_eligible=False)
    write(args.output/'report.json', report)
    status('complete', holdout_pass=gate['passed'])
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dataset', 'fit', 'cohort', 'output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    try:
        report = run(args)
        print(json.dumps(report), flush=True)
    except Exception as error:
        path = args.output/'status.json'
        if path.exists():
            state = json.loads(path.read_text())
            if state.get('pid') == os.getpid():
                state.update(stage='failed', error=repr(error), updated_unix=time.time())
                write(path, state)
        raise
