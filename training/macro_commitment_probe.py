"""Bounded CPU probe of persistent next-production intents on train games.

Targets may use future commands; every input is the unmodified current causal
feature row. This is a learning probe, without export or live control support.
"""
import argparse
from collections import Counter
import json
import os
from pathlib import Path
import time

import numpy as np
import torch
from torch import nn
from torch.nn import functional as F

from .schema import load_schema, sha256
from .shards import TensorShards


def commitment_targets(rows, horizon=120):
    """Next observed macro event within the horizon, or a fully observed wait.

    Windows crossing a sampling gap/end without a known event are censored.
    A currently masked future intent is censored, not relabeled as waiting.
    """
    frames, labels, action_frames = rows['frames'], rows['labels'], rows['action_frames']
    if horizon < 24 or len(frames) == 0 or (np.diff(frames) <= 0).any():
        raise ValueError('invalid horizon or sequence chronology')
    positive = np.flatnonzero(labels != 0)
    if (action_frames[positive] < frames[positive]).any():
        raise ValueError('positive event precedes its observation')
    if (np.diff(action_frames[positive]) < 0).any():
        raise ValueError('out-of-order event labels')
    targets = np.zeros(len(frames), dtype=np.int64)
    valid = np.zeros(len(frames), dtype=bool)
    event_at = np.searchsorted(action_frames[positive], frames)
    next_gap = np.full(len(frames), int(frames[-1]), dtype=np.int64)
    boundary = int(frames[-1])
    for index in range(len(frames)-2,-1,-1):
        if frames[index+1] - frames[index] > 24:
            boundary = int(frames[index]) + 24
        next_gap[index] = boundary
    for i, at in enumerate(event_at):
        if at < len(positive):
            event = positive[at]
            when = int(action_frames[event])
            if when <= frames[i] + horizon and when < next_gap[i]:
                target = int(labels[event])
                if (int(rows['masks'][i]) >> target) & 1:
                    targets[i], valid[i] = target, True
                continue
        valid[i] = frames[i] + horizon < next_gap[i]
    return targets, valid


class CommitmentModel(nn.Module):
    def __init__(self, features, actions, width=256):
        super().__init__()
        self.encoder = nn.Sequential(nn.Linear(features,width),nn.ReLU(),
                                     nn.Linear(width,width),nn.ReLU())
        self.event = nn.Linear(width,1)
        self.intent = nn.Linear(width,actions-1)

    def forward(self, x):
        state = self.encoder(x)
        return self.event(state).squeeze(-1), self.intent(state)


def metrics(predicted, labels, names):
    event, truth = predicted != 0, labels != 0
    tp = int((event & truth).sum())
    return dict(rows=len(labels), events=int(truth.sum()),
        event_precision=tp/max(1,int(event.sum())), event_recall=tp/max(1,int(truth.sum())),
        event_intent_accuracy=int(((predicted==labels)&truth).sum())/max(1,int(truth.sum())),
        per_action={name:dict(samples=int((labels==i).sum()), correct=int(((labels==i)&(predicted==i)).sum()))
                    for i,name in enumerate(names)},
        predicted_counts=dict(Counter(names[i] for i in predicted.tolist())))


@torch.no_grad()
def evaluate(model, data, names):
    x, masks, labels, games = data
    model.eval()
    results=[]
    for first in range(0,len(labels),1024):
        e, a = model(x[first:first+1024])
        allowed = masks[first:first+1024,1:]
        choice = a.masked_fill(~allowed,-torch.inf).argmax(-1)+1
        results.append(torch.where((e.sigmoid()>=.5)&allowed.any(-1),choice,0))
    prediction=torch.cat(results)
    report=metrics(prediction,labels,names)
    report['per_game']={g:metrics(prediction[games==g],labels[games==g],names)
                        for g in games.unique().tolist()}
    return report


def write(path, value):
    temp=path.with_suffix('.tmp.json')
    temp.write_text(json.dumps(value,indent=2)+'\n',encoding='utf-8')
    os.replace(temp,path)


def load_cohort(dataset, ids, horizon, max_frame=None):
    xs, ms, ys, gs = [], [], [], []
    counts=Counter()
    for sequence in dataset.sequences:
        if sequence['game_id'] not in ids:
            continue
        if sequence['split'] != 'train':
            raise ValueError('only train games allowed in the development probe')
        rows=dataset.read_rows(sequence['start'],sequence['count'])
        target,valid=commitment_targets(rows,horizon)
        if max_frame is not None:
            valid &= rows['frames'] < max_frame
        masks=((rows['masks'][:,None]>>np.arange(len(load_schema()['actions']),dtype=np.uint64))&1).astype(bool)
        xs.append(torch.from_numpy(rows['features'][valid]));ms.append(torch.from_numpy(masks[valid]))
        ys.append(torch.from_numpy(target[valid]));gs.append(torch.from_numpy(rows['game_indices'][valid].astype(np.int64)))
        counts[sequence['game_id']]+=int(valid.sum())
    if set(counts)!=set(ids) or not all(counts.values()):
        raise ValueError('missing/censored cohort game')
    return tuple(torch.cat(items) for items in (xs,ms,ys,gs)),dict(counts)


def run(args):
    if args.steps < 1 or (args.max_frame is not None and args.max_frame < 1):
        raise ValueError('positive step/frame limits required')
    if args.output.exists():
        raise FileExistsError(args.output)
    torch.set_num_threads(2)
    torch.manual_seed(20260924)
    rng=torch.Generator().manual_seed(20260924)
    cohort=json.loads(args.cohort.read_text())
    train_ids={g for ids in cohort['train_games'].values() for g in ids}
    dev_ids={g for ids in cohort['dev_games'].values() for g in ids}
    if train_ids & dev_ids:
        raise ValueError('overlapping train/development games')
    args.output.mkdir(parents=True)
    spec=dict(schema='protodd-macro-commitment-probe-v1',device='cpu',threads=2,
        source_sha256={str(p.name):sha256(p) for p in Path(__file__).parent.glob('*.py')},
        cohort_sha256=sha256(args.cohort),dataset_manifest_sha256=sha256(args.dataset/'manifest.json'),
        train_games=sorted(train_ids),dev_games=sorted(dev_ids),horizon_frames=120,
        steps=args.steps,batch_size=512,width=256,learning_rate=.0003,seed=20260924,
        max_frame=args.max_frame,balanced_types=args.balanced_types,
        criteria=dict(event_precision_min=.5,event_recall_min=.5,event_intent_accuracy_min=.25,
            reference_margin_min=.05,probe_recall_min=.5,pylon_recall_min=.2,gateway_recall_min=.2),
        inputs='current unmodified causal macro observation; future only in targets',
        inference_threshold=.5,promotion_eligible=False,live_control_allowed=False,
        purpose='bounded training of first economy/production scope, not a full-corpus fit')
    write(args.output/'run.json',spec)
    def status(stage,**more):
        write(args.output/'status.json',dict(stage=stage,pid=os.getpid(),unix=time.time(),**more))
    status('verifying_tensors')
    with TensorShards(args.dataset,progress=lambda done,total:status('verifying_tensors',verified=done,total=total)) as dataset:
        train,train_counts=load_cohort(dataset,train_ids,120,args.max_frame)
        dev,dev_counts=load_cohort(dataset,dev_ids,120,args.max_frame)
    names=load_schema()['actions']
    model=CommitmentModel(train[0].shape[1],len(names))
    optimizer=torch.optim.AdamW(model.parameters(),lr=.0003)
    write(args.output/'selection.json',dict(train_rows_by_game=train_counts,dev_rows_by_game=dev_counts))
    prior=torch.bincount(train[2],minlength=len(names)).float()
    type_weights=(prior[1:].sum()/prior[1:].clamp(min=1)).sqrt().clamp(max=8)
    type_weights=type_weights/(type_weights*prior[1:]).sum()*prior[1:].sum()
    if not args.balanced_types:
        type_weights=torch.ones_like(type_weights)
    write(args.output/'type_weights.json',dict(zip(names[1:],type_weights.tolist())))
    nonwait=prior[1:][None].expand(len(dev[2]),-1).masked_fill(~dev[1][:,1:],-1)
    baseline=nonwait.argmax(-1)+1
    action_ready_reference=metrics(torch.where(dev[1][:,1:].any(-1),baseline,0),dev[2],names)
    # A frozen training-only frequency reference predicts action readiness from
    # the train cohort's prevalence, with the same current intent mask.
    baseline=torch.where((dev[1][:,1:].any(-1))&(float((train[2]!=0).float().mean())>=.5),baseline,0)
    reference=metrics(baseline,dev[2],names)
    reference['action_ready_reference']=action_ready_reference
    write(args.output/'reference.json',reference)
    game_indices=[torch.nonzero(train[3]==g).flatten() for g in train[3].unique()]
    presented=torch.zeros(len(train[2]),dtype=torch.int32)
    began=time.monotonic()
    model.train()
    for step in range(1,args.steps+1):
        groups=torch.randint(len(game_indices),(512,),generator=rng)
        indices=torch.empty(512,dtype=torch.long)
        for g,eligible in enumerate(game_indices):
            where=torch.nonzero(groups==g).flatten()
            indices[where]=eligible[torch.randint(len(eligible),(len(where),),generator=rng)]
        x,m,y,_=(v[indices] for v in train)
        e,a=model(x)
        event=y!=0
        loss=F.binary_cross_entropy_with_logits(e,event.float())
        if event.any():
            loss=loss+F.cross_entropy(a[event].masked_fill(~m[event,1:],-torch.inf),y[event]-1,weight=type_weights)
        if not torch.isfinite(loss):
            raise ValueError('nonfinite scoped loss')
        optimizer.zero_grad(set_to_none=True);loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(),1.)
        optimizer.step()
        presented.index_add_(0,indices,torch.ones_like(indices,dtype=torch.int32))
        if step%100==0 or step==args.steps:
            state=dict(step=step,total_steps=args.steps,loss=float(loss.detach()),seconds=time.monotonic()-began,
                unique_rows=int((presented>0).sum()),presentations=int(presented.sum()))
            status('training',**state);print(json.dumps(state),flush=True)
            temp=args.output/'resume.tmp.pt'
            torch.save(dict(model=model.state_dict(),optimizer=optimizer.state_dict(),step=step,
                sampler_rng=rng.get_state(),torch_rng=torch.get_rng_state(),presented=presented),temp)
            os.replace(temp,args.output/'resume.pt')
    status('evaluating')
    result=evaluate(model,dev,names)
    def recall(action):
        row=result['per_action'][action]
        return row['correct']/max(1,row['samples'])
    checks=dict(event_precision=result['event_precision']>=.5,event_recall=result['event_recall']>=.5,
        intent_accuracy=result['event_intent_accuracy']>=max(.25,reference['event_intent_accuracy']+.05,
            action_ready_reference['event_intent_accuracy']+.05),
        probe=recall('train_probe')>=.5,pylon=recall('build_pylon')>=.2,gateway=recall('build_gateway')>=.2)
    report=dict(reference=reference,development=result,checks=checks,passed=all(checks.values()),
        unique_gradient_rows=int((presented>0).sum()),presentations=int(presented.sum()),
        train_rows=len(train[2]),dev_rows=len(dev[2]),promotion_eligible=False,
        execution_contract_tested=False,live_control_allowed=False,
        interpretation='A pass permits larger scoped development and execution tests; it is not playing-strength evidence.')
    write(args.output/'report.json',report)
    status('complete',passed=report['passed'])
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('dataset','cohort','output'):
        parser.add_argument(name,type=Path)
    parser.add_argument('--steps',type=int,default=1200)
    parser.add_argument('--max-frame',type=int)
    parser.add_argument('--balanced-types',action='store_true')
    args=parser.parse_args()
    try:
        report=run(args)
        print(json.dumps(dict(passed=report['passed'],checks=report['checks'])),flush=True)
    except Exception as error:
        if (args.output/'status.json').exists():
            state=json.loads((args.output/'status.json').read_text())
            if state.get('pid')==os.getpid():
                state.update(stage='failed',error=repr(error),unix=time.time())
                write(args.output/'status.json',state)
        raise
