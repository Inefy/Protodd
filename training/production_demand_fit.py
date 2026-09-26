"""Bounded local learning of simultaneous accepted-production demands.

Offline research only: no export, BWAPI commands or promotion authorization.
"""
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
from .production_demands import ACTIONS, HORIZON, MAX_COUNT, collect
from .schema import sha256
from .shards import TensorShards

GATES = dict(macro_f1_min=.45, macro_f1_reference_margin=.05, count_mae_reference_ratio=.95,
    probe_precision=.70, probe_recall=.60, pylon_precision=.50, pylon_recall=.40,
    gateway_precision=.45, gateway_recall=.30, army_precision=.40, army_recall=.30,
    required_type_support=10)


class DemandModel(nn.Module):
    def __init__(self, mean, scale, width=192):
        super().__init__()
        self.register_buffer('mean',mean)
        self.register_buffer('scale',scale)
        self.net=nn.Sequential(nn.Linear(len(mean),width),nn.ReLU(),
            nn.Linear(width,width),nn.ReLU(),nn.Linear(width,len(ACTIONS)*MAX_COUNT))

    def forward(self,x):
        raw=self.net((x-self.mean)/self.scale).reshape(-1,len(ACTIONS),MAX_COUNT)
        # Monotonic P(count >= k), with one independently learned demand per type.
        return torch.cat((raw[:,:,:1],raw[:,:,:1]-F.softplus(raw[:,:,1:]).cumsum(-1)),dim=-1)


def scores(prediction,truth):
    prediction,truth=np.asarray(prediction),np.asarray(truth)
    per={}
    for j,name in enumerate(ACTIONS):
        p,t=prediction[:,j]>0,truth[:,j]>0
        tp=int((p&t).sum());precision=tp/max(1,int(p.sum()));recall=tp/max(1,int(t.sum()))
        per[name]=dict(positive_rows=int(t.sum()),predicted_rows=int(p.sum()),true_positive=tp,
            precision=precision,recall=recall,f1=2*precision*recall/max(1e-12,precision+recall),
            count_mae=float(np.abs(prediction[:,j]-truth[:,j]).mean()))
    return dict(rows=len(truth),macro_f1=sum(v['f1'] for v in per.values())/len(ACTIONS),
        count_mae=float(np.abs(prediction-truth).mean()),
        exact_count_fraction=float((prediction==truth).mean()),
        exact_vector_fraction=float((prediction==truth).all(1).mean()),per_type=per,
        overflow_targets=int((truth>MAX_COUNT).sum()),
        concurrent_rows=int(((truth>0).sum(1)>1).sum()))


def gates(candidate,references):
    best_f1=max(r['macro_f1'] for r in references.values())
    best_mae=min(r['count_mae'] for r in references.values())
    checks=dict(macro_f1=candidate['macro_f1']>=max(GATES['macro_f1_min'],best_f1+GATES['macro_f1_reference_margin']),
        count_mae=candidate['count_mae']<=best_mae*GATES['count_mae_reference_ratio'])
    for name,key in (('train_probe','probe'),('build_pylon','pylon'),('build_gateway','gateway'),
                     ('train_zealot','army'),('train_dragoon','army')):
        row=candidate['per_type'][name]
        checks[name]=bool(row['positive_rows']>=GATES['required_type_support']
            and row['precision']>=GATES[key+'_precision'] and row['recall']>=GATES[key+'_recall'])
    return dict(passed=all(checks.values()),checks=checks)


@torch.no_grad()
def evaluate(model,data,device):
    model.eval();predictions=[]
    for start in range(0,len(data['counts']),2048):
        logits=model(torch.from_numpy(data['features'][start:start+2048]).to(device))
        predictions.append((logits>=0).sum(-1).cpu().numpy())
    predicted=np.concatenate(predictions)
    report=scores(predicted,data['counts'])
    report['per_game']={str(g):scores(predicted[data['games']==g],data['counts'][data['games']==g])
        for g in np.unique(data['games'])}
    return report


def run(args):
    if args.steps<1 or args.output.exists():
        raise ValueError('positive budget and unused output directory required')
    torch.set_num_threads(2);torch.manual_seed(20260924)
    generator=torch.Generator().manual_seed(20260924)
    args.output.mkdir(parents=True)
    def status(stage,**values):
        write(args.output/'status.json',dict(stage=stage,pid=os.getpid(),updated_unix=time.time(),**values))
    cohort=json.loads(args.cohort.read_text())
    train_ids={g for ids in cohort['train_games'].values() for g in ids}
    dev_ids={g for ids in cohort['dev_games'].values() for g in ids}
    if train_ids&dev_ids: raise ValueError('cohort overlap')
    spec=dict(schema='protodd-concurrent-production-fit-v1',steps=args.steps,capacity=args.capacity,
        cohort_sha256=sha256(args.cohort),dataset_manifest_sha256=sha256(args.dataset/'manifest.json'),
        source_sha256={p.name:sha256(p) for p in sorted(Path(__file__).parent.glob('*.py'))},
        schema_sha256=sha256(Path(__file__).with_name('schema_v2.json')),
        train_games=sorted(train_ids),dev_games=sorted(dev_ids),seed=20260924,
        device=args.device,width=192,batch_size=512,learning_rate=.0003,max_frame=7200,
        horizon=HORIZON,max_count=MAX_COUNT,actions=ACTIONS,development_gates=GATES,
        capacity_gates=dict(exact_count_min=.95,macro_f1_min=.85),
        decoding='count of monotonic ordinal logits >= 0; no tuning on development',
        input_contract='current causal macro features plus strictly past accepted own action counts/ages',
        target_contract='all accepted scope commands in [frame, frame+240); acceptance is not completion',
        promotion_eligible=False,live_control_allowed=False)
    write(args.output/'run.json',spec)
    status('verifying_tensors')
    with TensorShards(args.dataset,progress=lambda done,total:status('verifying_tensors',done=done,total=total)) as dataset:
        train,train_detail,train_receipts=collect(dataset,train_ids,progress=lambda done,total:
            status('collecting_train',games=done,total=total))
        dev,dev_detail,dev_receipts=collect(dataset,dev_ids,progress=lambda done,total:
            status('collecting_development',games=done,total=total))
    for field in ('duplicate_group','replay_sha256'):
        if {v[field] for v in train_receipts.values()} & {v[field] for v in dev_receipts.values()}:
            raise ValueError('duplicate replay crosses cohorts')
    chosen=np.arange(len(train['counts']))
    if args.capacity:
        rng=np.random.default_rng(20260924)
        # Cover each positive type and quiet rows; fill deterministically to 512.
        picked=set()
        for j in range(len(ACTIONS)):
            eligible=np.flatnonzero(train['counts'][:,j]>0)
            picked.update(rng.choice(eligible,min(48,len(eligible)),replace=False).tolist())
        pool=np.array(sorted(set(chosen)-picked))
        picked.update(rng.choice(pool,512-len(picked),replace=False).tolist())
        chosen=np.array(sorted(picked))
        train={k:v[chosen] for k,v in train.items()}
    selection=dict(train_rows_by_game=train_detail,dev_rows_by_game=dev_detail,
        train_receipts=train_receipts,dev_receipts=dev_receipts,
        train_keys=[[int(g),int(p),int(f)] for g,p,f in zip(train['games'],train['perspectives'],train['frames'])],
        dev_keys=[[int(g),int(p),int(f)] for g,p,f in zip(dev['games'],dev['perspectives'],dev['frames'])])
    write(args.output/'selection.json',selection)
    # Cache the exact selected supervised tensors for reproducible audits.
    for split,data in (('train',train),('dev',dev)):
        np.savez_compressed(args.output/(split+'.npz'),**data)
    write(args.output/'data.json',{split:sha256(args.output/(split+'.npz')) for split in ('train','dev')})
    x=torch.from_numpy(train['features']).to(args.device)
    truth=torch.from_numpy(train['counts']).to(args.device)
    target=(truth[:,:,None]>torch.arange(MAX_COUNT,device=args.device)).float()
    model=DemandModel(x.mean(0),x.std(0).clamp(min=.02)).to(args.device)
    optimizer=torch.optim.AdamW(model.parameters(),lr=.0003)
    baseline={
        'zero':scores(np.zeros_like(dev['counts']),dev['counts']),
        'train_median':scores(np.broadcast_to(np.median(train['counts'],axis=0).astype(np.int64),dev['counts'].shape),dev['counts']),
        'repeat_previous_240_frames':scores(np.minimum(dev['previous'],MAX_COUNT),dev['counts'])}
    write(args.output/'reference.json',baseline)
    presented=torch.zeros(len(x),dtype=torch.int32)
    # Equal sampling probability for each game, then each selected row in it.
    unique_games,game_counts=np.unique(train['games'],return_counts=True)
    frequency=dict(zip(unique_games.tolist(),game_counts.tolist()))
    sample_weights=torch.tensor([1/frequency[int(g)] for g in train['games']],dtype=torch.double)
    started=time.monotonic();model.train()
    for step in range(1,args.steps+1):
        cpu_indices=torch.multinomial(sample_weights,512,replacement=True,generator=generator)
        indices=cpu_indices.to(args.device)
        loss=F.binary_cross_entropy_with_logits(model(x[indices]),target[indices])
        if not torch.isfinite(loss): raise ValueError('nonfinite production loss')
        optimizer.zero_grad(set_to_none=True);loss.backward()
        torch.nn.utils.clip_grad_norm_(model.parameters(),1.0);optimizer.step()
        presented.index_add_(0,cpu_indices,torch.ones_like(cpu_indices,dtype=torch.int32))
        if step%100==0 or step==args.steps:
            row=dict(step=step,total_steps=args.steps,loss=float(loss.detach()),seconds=time.monotonic()-started,
                unique_rows=int((presented>0).sum()),presentations=int(presented.sum()))
            status('training',**row);print(json.dumps(row),flush=True)
            temporary=args.output/'resume.tmp.pt'
            torch.save(dict(model=model.state_dict(),optimizer=optimizer.state_dict(),step=step,
                sampler_rng=generator.get_state(),torch_rng=torch.get_rng_state(),
                presented=presented),temporary)
            os.replace(temporary,args.output/'resume.pt')
    status('evaluating')
    after=dict(train=evaluate(model,train,args.device),dev=evaluate(model,dev,args.device))
    checks=gates(after['dev'],baseline)
    capacity_pass=after['train']['exact_count_fraction']>=.95 and after['train']['macro_f1']>=.85
    report=dict(after=after,development=checks,capacity_pass=capacity_pass,steps=args.steps,
        unique_gradient_rows=int((presented>0).sum()),presentations=int(presented.sum()),
        train_rows=len(x),dev_rows=len(dev['counts']),
        counts_are_acceptances_not_completions=True,execution_contract_tested=False,
        promotion_eligible=False,live_control_allowed=False)
    write(args.output/'report.json',report)
    status('complete',capacity_pass=capacity_pass,development_pass=checks['passed'])
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('dataset','cohort','output'):parser.add_argument(name,type=Path)
    parser.add_argument('--steps',type=int,default=3200)
    parser.add_argument('--capacity',action='store_true')
    parser.add_argument('--device',choices=('cpu','cuda'),default='cuda')
    args=parser.parse_args()
    try:
        report=run(args)
        print(json.dumps(dict(capacity_pass=report['capacity_pass'],development=report['development'])),flush=True)
    except Exception as error:
        path=args.output/'status.json'
        if path.exists():
            state=json.loads(path.read_text())
            if state.get('pid')==os.getpid():
                state.update(stage='failed',error=repr(error),updated_unix=time.time());write(path,state)
        raise
