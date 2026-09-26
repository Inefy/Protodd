"""Fixed-weight confirmation on reserved validation games, never final test."""
import argparse
import json
import os
from pathlib import Path
import time

import numpy as np
import torch

from .macro_commitment_probe import write
from .production_demands import collect,MAX_COUNT
from .production_demand_fit import DemandModel,evaluate,gates,scores,GATES
from .schema import sha256
from .shards import TensorShards


def run(args):
    if args.output.exists():raise FileExistsError(args.output)
    spec=json.loads((args.fit/'run.json').read_text())
    previous=json.loads((args.fit/'report.json').read_text())
    inventory=json.loads(args.inventory.read_text())
    if spec['capacity'] or not previous['development']['passed']:
        raise ValueError('development candidate must pass before confirmation')
    if sha256(Path(__file__).with_name('production_demand_fit.py'))!=spec['source_sha256']['production_demand_fit.py']:
        raise ValueError('model, decoder or gates differ from fitted source')
    if sha256(args.dataset/'manifest.json')!=spec['dataset_manifest_sha256']:
        raise ValueError('tensor release changed')
    chosen=inventory['selected']
    if len(chosen)!=24 or any(row['split']!='validation' for row in chosen):
        raise ValueError('expected reserved 24-game validation cohort')
    ids={row['game_id'] for row in chosen}
    if len(ids)!=24 or ids & set(spec['train_games']+spec['dev_games']):
        raise ValueError('confirmation overlap')
    checkpoint_sha=sha256(args.fit/'resume.pt')
    args.output.mkdir(parents=True)
    frozen=dict(schema='protodd-production-demand-confirmation-v1',checkpoint_sha256=checkpoint_sha,
        fit_run_sha256=sha256(args.fit/'run.json'),fit_report_sha256=sha256(args.fit/'report.json'),
        inventory_sha256=sha256(args.inventory),selected=chosen,criteria=GATES,
        source_sha256={p.name:sha256(p) for p in sorted(Path(__file__).parent.glob('*.py'))},
        schema_sha256=sha256(Path(__file__).with_name('schema_v2.json')),
        parameters_frozen_before_payloads=True,thresholds_changed=False,
        globally_fresh=False,prior_macro_validation_exposure=True,
        additional_optimizer_updates=0,promotion_eligible=False,live_control_allowed=False)
    write(args.output/'run.json',frozen)
    def status(stage,**values):
        write(args.output/'status.json',dict(stage=stage,pid=os.getpid(),updated_unix=time.time(),**values))
    status('verifying_tensors')
    torch.set_num_threads(2)
    with TensorShards(args.dataset,progress=lambda done,total:status('verifying_tensors',done=done,total=total)) as dataset:
        data,detail,receipts=collect(dataset,ids,split='validation',
            perspectives={row['game_id']:row['perspective'] for row in chosen},
            progress=lambda done,total:status('collecting_confirmation',games=done,total=total))
        game_matchups={i:g['matchup'] for i,g in enumerate(dataset.games)}
    prior_selection=json.loads((args.fit/'selection.json').read_text())
    for field in ('duplicate_group','replay_sha256'):
        used={v[field] for collection in ('train_receipts','dev_receipts') for v in prior_selection[collection].values()}
        if used&{v[field] for v in receipts.values()}:raise ValueError('confirmation duplicate detected')
    for row in chosen:
        if receipts[row['game_id']]['replay_sha256']!=row['replay_sha256']:
            raise ValueError('reserved replay identity changed')
    write(args.output/'selection.json',dict(rows_by_game=detail,receipts=receipts))
    np.savez_compressed(args.output/'confirmation.npz',**data)
    write(args.output/'data.json',dict(confirmation=sha256(args.output/'confirmation.npz')))
    train_receipt=json.loads((args.fit/'data.json').read_text())
    if sha256(args.fit/'train.npz')!=train_receipt['train']:raise ValueError('training baseline cache changed')
    with np.load(args.fit/'train.npz',allow_pickle=False) as train:
        median=np.median(train['counts'],axis=0).astype(np.int64)
    references={
        'zero':scores(np.zeros_like(data['counts']),data['counts']),
        'train_median':scores(np.broadcast_to(median,data['counts'].shape),data['counts']),
        'repeat_previous_240_frames':scores(np.minimum(data['previous'],MAX_COUNT),data['counts'])}
    checkpoint=torch.load(args.fit/'resume.pt',map_location='cpu',weights_only=True)
    if checkpoint['step']!=spec['steps'] or sha256(args.fit/'resume.pt')!=checkpoint_sha:
        raise ValueError('checkpoint changed during confirmation')
    state=checkpoint['model'];model=DemandModel(state['mean'],state['scale'],spec['width']).to('cuda')
    model.load_state_dict(state)
    status('evaluating')
    result=evaluate(model,data,'cuda')
    checks=gates(result,references)
    matchups={}
    for name in ('PvP','PvT','PvZ'):
        mask=np.array([game_matchups[int(g)]==name for g in data['games']])
        matchups[name]=evaluate(model,{k:v[mask] for k,v in data.items()},'cuda')
    report=dict(confirmation=result,references=references,by_matchup=matchups,gate=checks,
        checkpoint_sha256=checkpoint_sha,additional_optimizer_updates=0,
        globally_fresh=False,prior_macro_validation_exposure=True,final_test_payloads_opened=False,
        promotion_eligible=False,live_control_allowed=False,
        note='Pass permits native/execution work only. No measured playing-strength improvement.')
    write(args.output/'report.json',report);status('complete',passed=checks['passed'])
    print(json.dumps(checks),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('fit','dataset','inventory','output'):parser.add_argument(name,type=Path)
    args=parser.parse_args()
    try:run(args)
    except Exception as error:
        path=args.output/'status.json'
        if path.exists():
            state=json.loads(path.read_text())
            if state.get('pid')==os.getpid():
                state.update(stage='failed',error=repr(error),updated_unix=time.time());write(path,state)
        raise
