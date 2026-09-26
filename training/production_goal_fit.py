"""Bounded population-goal and spending-priority experiment. No live export."""
import argparse
import json
import os
from pathlib import Path
import time

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

from .macro_commitment_probe import write
from .production_goals import UNITS, HORIZON, collect
from .schema import sha256
from .shards import TensorShards

SEED = 20260924
GATES = dict(normalized_mae_reference_ratio=.95, per_unit_mae_reference_ratio=1.05,
             probe_precision=.75, probe_recall=.75, army_precision=.50, army_recall=.50,
             priority_accuracy_margin=.05, priority_macro_recall=.40,
             late_probe_recall=.75, late_probe_precision=.75, required_positive_rows=20)


class GoalModel(nn.Module):
    def __init__(self, mean, scale, width=192):
        super().__init__()
        self.register_buffer('mean', mean)
        self.register_buffer('scale', scale)
        self.net = nn.Sequential(nn.Linear(len(mean), width), nn.ReLU(),
                                 nn.Linear(width, width), nn.ReLU())
        self.goals = nn.Linear(width, 3)
        self.priority = nn.Linear(width, 3)

    def forward(self, x):
        h = self.net((x-self.mean)/self.scale)
        return self.goals(h), self.priority(h)


def score(predicted, priority, data):
    predicted = np.maximum(np.rint(predicted), 0).astype(np.int64)
    truth, stock = data['goals'], data['committed']
    error = np.abs(predicted-truth)
    def growth(mask, j):
        p, t = predicted[mask, j] > stock[mask, j], truth[mask, j] > stock[mask, j]
        tp = int((p&t).sum())
        return dict(positive_rows=int(t.sum()), predicted_rows=int(p.sum()),
                    precision=tp/max(1, int(p.sum())), recall=tp/max(1, int(t.sum())))
    all_rows = np.ones(len(truth), dtype=bool)
    use = data['priority'] >= 0
    recalls = [float((priority[use & (data['priority'] == j)] == j).mean())
               if (use & (data['priority'] == j)).any() else 0. for j in range(3)]
    return dict(rows=len(truth), normalized_mae=float((error/np.array([8, 4, 4])).mean()),
                per_unit={u: dict(mae=float(error[:, j].mean()), **growth(all_rows, j)) for j, u in enumerate(UNITS)},
                late_probe=growth(data['frames'] >= 4800, 0),
                priority_rows=int(use.sum()), priority_accuracy=float((priority[use] == data['priority'][use]).mean()) if use.any() else 0.,
                priority_macro_recall=float(np.mean(recalls)), priority_recalls=recalls,
                excessive_goal_rows=int((predicted > np.array([80, 60, 60])).any(1).sum()))


def gates(candidate, references):
    checks = dict(normalized_mae=candidate['normalized_mae'] <= GATES['normalized_mae_reference_ratio'] * min(r['normalized_mae'] for r in references.values()),
                  priority_accuracy=candidate['priority_accuracy'] >= max(r['priority_accuracy'] for r in references.values())+GATES['priority_accuracy_margin'],
                  priority_macro_recall=candidate['priority_macro_recall'] >= GATES['priority_macro_recall'],
                  bounded_goals=candidate['excessive_goal_rows'] == 0)
    for u in UNITS:
        row = candidate['per_unit'][u]
        prefix = 'probe' if u == 'probe' else 'army'
        checks[u+'_mae'] = row['mae'] <= GATES['per_unit_mae_reference_ratio']*min(r['per_unit'][u]['mae'] for r in references.values())
        checks[u+'_growth'] = (row['positive_rows'] >= GATES['required_positive_rows'] and row['precision'] >= GATES[prefix+'_precision'] and row['recall'] >= GATES[prefix+'_recall'])
    row = candidate['late_probe']
    checks['late_probe_growth'] = row['positive_rows'] >= GATES['required_positive_rows'] and row['precision'] >= GATES['late_probe_precision'] and row['recall'] >= GATES['late_probe_recall']
    return dict(passed=all(checks.values()), checks=checks)


@torch.no_grad()
def evaluate(model, data, device):
    model.eval()
    goals, priorities = [], []
    for first in range(0, len(data['goals']), 2048):
        g, p = model(torch.from_numpy(data['features'][first:first+2048]).to(device))
        goals.append(g.cpu().numpy()*np.array([8, 4, 4]))
        priorities.append(p.argmax(-1).cpu().numpy())
    goals, priorities = np.concatenate(goals), np.concatenate(priorities)
    result = score(goals, priorities, data)
    result['per_game'] = {str(g): score(goals[data['games'] == g], priorities[data['games'] == g],
                                      {k: v[data['games'] == g] for k, v in data.items()}) for g in np.unique(data['games'])}
    return result


def references(train, dev, device):
    valid = train['priority'] >= 0
    majority = int(np.bincount(train['priority'][valid], minlength=3).argmax())
    priorities = np.full(len(dev['goals']), majority)
    result = dict(persistence=score(dev['committed'], priorities, dev))
    # A strong low-capacity baseline fit only on the exact training rows.
    x = torch.from_numpy(train['features']).to(device=device, dtype=torch.float64)
    mean, scale = x.mean(0), x.std(0).clamp(min=.02)
    x = torch.cat(((x-mean)/scale, torch.ones((len(x), 1), device=device, dtype=x.dtype)), 1)
    y = torch.from_numpy(train['goals']).to(device=device, dtype=x.dtype)
    regularizer = torch.eye(x.shape[1], device=device, dtype=x.dtype)*len(x)*.01
    regularizer[-1, -1] = 0
    w = torch.linalg.solve(x.T@x+regularizer, x.T@y)
    dx = torch.from_numpy(dev['features']).to(device=device, dtype=x.dtype)
    dx = torch.cat(((dx-mean)/scale, torch.ones((len(dx), 1), device=device, dtype=x.dtype)), 1)
    result['ridge_population'] = score((dx@w).cpu().numpy(), priorities, dev)
    # Time/race population and priority medians test whether the net merely
    # learns the replay clock. Empty cells fall back to global train medians.
    def bins(d):
        return (d['frames']//1200)*3+d['features'][:, 9:12].argmax(1)
    tb, db = bins(train), bins(dev)
    goal = np.broadcast_to(np.median(train['goals'], axis=0), dev['goals'].shape).copy()
    for key in np.unique(tb):
        selected = tb == key
        goal[db == key] = np.median(train['goals'][selected], axis=0)
        p = train['priority'][selected & valid]
        if len(p):
            priorities[db == key] = np.bincount(p, minlength=3).argmax()
    result['time_race_median'] = score(goal, priorities, dev)
    return result


def run(args):
    if args.steps < 1 or args.output.exists():
        raise ValueError('positive budget and unused output required')
    torch.set_num_threads(2)
    torch.manual_seed(SEED)
    generator = torch.Generator().manual_seed(SEED)
    args.output.mkdir(parents=True)
    def status(stage, **values):
        write(args.output/'status.json', dict(stage=stage, pid=os.getpid(), updated_unix=time.time(), **values))
    cohort = json.loads(args.cohort.read_text())
    ids = {s: {g for values in cohort[s+'_games'].values() for g in values} for s in ('train', 'dev')}
    if ids['train'] & ids['dev']:
        raise ValueError('cohort overlap')
    full_path = Path(cohort['excluded_full_fit_run'])
    if sha256(full_path) != cohort['excluded_full_fit_sha256']:
        raise ValueError('full-fit exclusion receipt changed')
    used = {g for group in json.loads(full_path.read_text())['groups'] for values in group.values() for g in values}
    if used & (ids['train'] | ids['dev']):
        raise ValueError('goal cohort was not omitted from full fit')
    spec = dict(schema='protodd-population-goals-fit-v1', steps=args.steps, capacity=args.capacity,
                horizon=HORIZON, max_frame=7200, seed=SEED, batch_size=512, width=192,
                learning_rate=.0003, device=args.device, gates=GATES,
                cohort_sha256=sha256(args.cohort), dataset_manifest_sha256=sha256(args.dataset/'manifest.json'),
                source_sha256={p.name: sha256(p) for p in Path(__file__).parent.glob('*.py')},
                train_games=sorted(ids['train']), dev_games=sorted(ids['dev']),
                target='peak completed population over next 1200 frames; first distinct accepted deficient-type priority',
                loss='smooth-L1 goals / [8,4,4] + 0.1 priority cross-entropy',
                decoding='round absolute goals, clamp only below zero; raw priority argmax audited separately; live executor would mask satisfied/illegal goals',
                capacity_gate='Probe MAE <= 0.5, each army MAE <= 0.3, priority accuracy >= 0.85',
                final_test_opened=False, evaluation_game_traces_used=False, live_control_allowed=False)
    write(args.output/'run.json', spec)
    data = {}
    if args.cache:
        cache_spec = json.loads((args.cache/'run.json').read_text())
        for key in ('cohort_sha256', 'dataset_manifest_sha256', 'horizon'):
            if cache_spec[key] != spec[key]:
                raise ValueError('goal cache identity differs')
        if cache_spec['source_sha256']['production_goals.py'] != spec['source_sha256']['production_goals.py']:
            raise ValueError('goal cache target source differs')
        digests = json.loads((args.cache/'data.json').read_text())
        for split in ids:
            path = args.cache/(split+'.npz')
            if sha256(path) != digests[split]:
                raise ValueError('goal tensor cache changed')
            with np.load(path, allow_pickle=False) as values:
                data[split] = dict(values)
        write(args.output/'cache.json', dict(path=str(args.cache.resolve()), data_sha256=sha256(args.cache/'data.json'), selection_sha256=sha256(args.cache/'selection.json')))
        write(args.output/'selection.json', json.loads((args.cache/'selection.json').read_text()))
    else:
        status('verifying_tensors')
        selection = {}
        with TensorShards(args.dataset, progress=lambda n, total: status('verifying_tensors', rows=n, total=total)) as dataset:
            for split in ids:
                data[split], detail, receipts = collect(dataset, ids[split], progress=lambda n, total: status('collecting_'+split, games=n, total=total))
                selection[split] = dict(rows_by_game=detail, receipts=receipts)
        for field in ('duplicate_group', 'replay_sha256'):
            if {v[field] for v in selection['train']['receipts'].values()} & {v[field] for v in selection['dev']['receipts'].values()}:
                raise ValueError('duplicate replay crosses cohorts')
        write(args.output/'selection.json', selection)
    for split, values in data.items():
        np.savez_compressed(args.output/(split+'.npz'), **values)
    write(args.output/'data.json', {s: sha256(args.output/(s+'.npz')) for s in ids})
    train, dev = data['train'], data['dev']
    if args.capacity:
        rng = np.random.default_rng(SEED)
        chosen = np.sort(rng.choice(len(train['goals']), min(512, len(train['goals'])), replace=False))
        train = {k: v[chosen] for k, v in train.items()}
        write(args.output/'capacity-rows.json', chosen.tolist())
    status('references')
    baseline = references(train, dev, args.device)
    write(args.output/'reference.json', baseline)
    x = torch.from_numpy(train['features']).to(args.device)
    y = torch.from_numpy(train['goals']).to(args.device).float()/torch.tensor([8, 4, 4], device=args.device)
    priority = torch.from_numpy(train['priority']).to(args.device)
    model = GoalModel(x.mean(0), x.std(0).clamp(min=.02)).to(args.device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=.0003)
    unique, counts = np.unique(train['games'], return_counts=True)
    frequency = dict(zip(unique.tolist(), counts.tolist()))
    weights = torch.tensor([1/frequency[int(g)] for g in train['games']], dtype=torch.double)
    presented = torch.zeros(len(x), dtype=torch.int32)
    started = time.monotonic()
    for step in range(1, args.steps+1):
        cpu = torch.multinomial(weights, 512, replacement=True, generator=generator)
        index = cpu.to(args.device)
        goal, ordering = model(x[index])
        use = priority[index] >= 0
        loss = F.smooth_l1_loss(goal, y[index])
        if use.any():
            loss = loss+.1*F.cross_entropy(ordering[use], priority[index][use])
        if not torch.isfinite(loss):
            raise ValueError('nonfinite goal loss')
        optimizer.zero_grad(set_to_none=True)
        loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.)
        optimizer.step()
        presented.index_add_(0, cpu, torch.ones_like(cpu, dtype=torch.int32))
        if step % 100 == 0 or step == args.steps:
            row = dict(step=step, total_steps=args.steps, loss=float(loss.detach()), seconds=time.monotonic()-started,
                       unique_rows=int((presented > 0).sum()), presentations=int(presented.sum()))
            status('training', **row)
            print(json.dumps(row), flush=True)
            tmp = args.output/'resume.tmp.pt'
            torch.save(dict(model=model.state_dict(), optimizer=optimizer.state_dict(), step=step,
                            sampler_rng=generator.get_state(), torch_rng=torch.get_rng_state(), presented=presented), tmp)
            os.replace(tmp, args.output/'resume.pt')
    status('evaluating')
    after = {s: evaluate(model, d, args.device) for s, d in (('train', train), ('dev', dev))}
    gate = gates(after['dev'], baseline)
    capacity_pass = all(after['train']['per_unit'][u]['mae'] <= (.5 if u == 'probe' else .3) for u in UNITS) and after['train']['priority_accuracy'] >= .85
    report = dict(after=after, development=gate, capacity_pass=capacity_pass, steps=args.steps,
                  unique_gradient_rows=int((presented > 0).sum()), presentations=int(presented.sum()),
                  train_rows=len(x), dev_rows=len(dev['goals']), live_control_allowed=False,
                  strength_proven=False, promotion_eligible=False)
    write(args.output/'report.json', report)
    status('complete', capacity_pass=capacity_pass, development_pass=gate['passed'])
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('dataset', 'cohort', 'output'):
        parser.add_argument(name, type=Path)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--capacity', action='store_true')
    parser.add_argument('--steps', type=int, default=3200)
    parser.add_argument('--device', choices=('cpu', 'cuda'), default='cuda')
    args = parser.parse_args()
    try:
        report = run(args)
        print(json.dumps(dict(capacity_pass=report['capacity_pass'], development=report['development'])), flush=True)
    except Exception as error:
        path = args.output/'status.json'
        if path.exists():
            state = json.loads(path.read_text())
            if state.get('pid') == os.getpid():
                state.update(stage='failed', error=repr(error), updated_unix=time.time())
                write(path, state)
        raise
