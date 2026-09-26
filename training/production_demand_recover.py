"""Finish evaluation of a fully fitted checkpoint using its frozen source.

No optimizer or fitting operation exists here. Invoked as a script so the
original source snapshot is placed first on the import path before importing it.
"""
import argparse
import json
from pathlib import Path
import os
import sys
import time


def recover(output,source):
    sys.path.insert(0,str(source.resolve()))
    import numpy as np
    import torch
    from training.production_demand_fit import DemandModel,evaluate,gates,scores
    from training.schema import sha256
    torch.set_num_threads(2)
    spec=json.loads((output/'run.json').read_text())
    for name,digest in spec['source_sha256'].items():
        if sha256(source/'training'/name)!=digest:
            raise ValueError('frozen source changed: '+name)
    if sha256(source/'training/schema_v2.json')!=spec['schema_sha256']:
        raise ValueError('schema changed')
    if (output/'report.json').exists():raise FileExistsError('evaluation already completed')
    previous=json.loads((output/'status.json').read_text())
    if previous['stage']!='failed':raise ValueError('recovery requires a verified exited failed job')
    checkpoint=torch.load(output/'resume.pt',map_location='cpu',weights_only=True)
    if checkpoint['step']!=spec['steps']:
        raise ValueError('fit is incomplete; evaluation-only recovery forbidden')
    data_receipt=json.loads((output/'data.json').read_text())
    data={}
    for split in ('train','dev'):
        path=output/(split+'.npz')
        if sha256(path)!=data_receipt[split]:raise ValueError('cached tensors changed')
        with np.load(path,allow_pickle=False) as archive:data[split]={k:archive[k] for k in archive.files}
    presented=checkpoint['presented']
    if len(presented)!=len(data['train']['counts']) or int(presented.sum())!=spec['steps']*spec['batch_size']:
        raise ValueError('checkpoint exposure mismatch')
    from training.production_demands import MAX_COUNT
    baseline={
        'zero':scores(np.zeros_like(data['dev']['counts']),data['dev']['counts']),
        'train_median':scores(np.broadcast_to(np.median(data['train']['counts'],axis=0).astype(np.int64),data['dev']['counts'].shape),data['dev']['counts']),
        'repeat_previous_240_frames':scores(np.minimum(data['dev']['previous'],MAX_COUNT),data['dev']['counts'])}
    if baseline!=json.loads((output/'reference.json').read_text()):
        raise ValueError('cached data reference differs')
    def write(path,value):
        temporary=path.with_suffix('.tmp.json')
        temporary.write_text(json.dumps(value,indent=2)+'\n')
        for attempt in range(20):
            try:os.replace(temporary,path);return
            except PermissionError:
                if attempt==19:raise
                time.sleep(.05)
    provenance=dict(previous_status=previous,checkpoint_sha256=sha256(output/'resume.pt'),
        frozen_source=str(source.resolve()),recovery_source_sha256=sha256(__file__),
        source_verified=True,cache_verified=True,additional_optimizer_updates=0)
    write(output/'recovery.json',provenance)
    write(output/'status.json',dict(stage='recovering_evaluation',pid=os.getpid(),updated_unix=time.time()))
    state=checkpoint['model']
    model=DemandModel(state['mean'],state['scale'],spec['width']).to(spec['device'])
    model.load_state_dict(state)
    after={split:evaluate(model,rows,spec['device']) for split,rows in data.items()}
    checks=gates(after['dev'],baseline)
    capacity_pass=after['train']['exact_count_fraction']>=.95 and after['train']['macro_f1']>=.85
    report=dict(after=after,development=checks,capacity_pass=capacity_pass,steps=checkpoint['step'],
        unique_gradient_rows=int((presented>0).sum()),presentations=int(presented.sum()),
        train_rows=len(data['train']['counts']),dev_rows=len(data['dev']['counts']),
        counts_are_acceptances_not_completions=True,execution_contract_tested=False,
        promotion_eligible=False,live_control_allowed=False,evaluation_recovery=provenance)
    write(output/'report.json',report)
    write(output/'status.json',dict(stage='complete',pid=os.getpid(),updated_unix=time.time(),
        capacity_pass=capacity_pass,development_pass=checks['passed'],recovered_evaluation=True))
    print(json.dumps(dict(capacity_pass=capacity_pass,development=checks)),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path);parser.add_argument('source',type=Path)
    args=parser.parse_args();recover(args.output,args.source)
