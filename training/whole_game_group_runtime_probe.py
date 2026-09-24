"""CPU-only offline parity/timing preflight; not the BWAPI callback timing gate."""
import argparse
import importlib.util
import json
from pathlib import Path
import time

import numpy as np
import torch

from .schema import sha256
from .whole_game_group_model import GroupCommandModel
from .whole_game_group_runtime import RollingGroupInference


def synthetic_batch(entities):
    own=entities//2
    numeric=torch.rand(1,entities,16)
    numeric[:,:,4]=1
    return dict(type=torch.randint(0,228,(1,entities)),
        relation=torch.cat((torch.zeros(1,own),torch.ones(1,entities-own)),1).long(),
        order=torch.randint(0,190,(1,entities)),entity_numeric=numeric,
        entity_mask=torch.ones(1,entities,dtype=torch.bool),
        spatial=torch.rand(1,3,128,128),global_state=torch.zeros(1,216))


def probe(checkpoint, frozen_source, output):
    if output.exists():
        raise FileExistsError(output)
    torch.set_num_threads(2)
    torch.manual_seed(42)
    spec=importlib.util.spec_from_file_location('training._frozen_group_runtime_baseline',frozen_source)
    module=importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    saved=torch.load(checkpoint,map_location='cpu',weights_only=True)
    old=module.GroupCommandModel(width=256).eval()
    new=GroupCommandModel(width=256).eval()
    old.load_state_dict(saved['model']);new.load_state_dict(saved['model'])
    measurements=[]
    with torch.no_grad():
        for count in (64,128,256,512):
            batch=synthetic_batch(count)
            batch['global']=batch.pop('global_state')
            before=old.forward_slots(batch)
            after=new.forward_slots(batch)
            for left,right in zip(before['slots'],after['slots']):
                for key in left:
                    torch.testing.assert_close(left[key],right[key],rtol=0,atol=0)
            adapter=RollingGroupInference(new)
            timings=[]
            for i in range(28):
                batch['global'][:,4]=i*24/100000
                began=time.perf_counter()
                result=adapter.step('synthetic',i*24,batch)
                elapsed=(time.perf_counter()-began)*1000
                if i>=8:
                    timings.append(elapsed)
            measurements.append(dict(entities=count,samples=len(timings),
                p50_ms=float(np.median(timings)),p95_ms=float(np.percentile(timings,95)),
                max_ms=max(timings)))
    report=dict(schema='protodd-group-runtime-preflight-v1',device='cpu',threads=2,
        checkpoint_sha256=sha256(checkpoint),frozen_model_source_sha256=sha256(frozen_source),
        current_sources={str(p):sha256(p) for p in (Path(__file__),Path('training/whole_game_group_model.py'),
            Path('training/whole_game_group_runtime.py'))},
        frozen_model_numeric_parity=True,rolling_history_contract='eight previous encodings, GRU rebuilt from zero',
        measurements=measurements,win32_parity=False,full_bwapi_callback_measured=False,
        includes_observation_encoding=False,synthetic_inputs=True,
        environment='local development PC while a separate GPU training job is active',
        promotion_eligible=False,live_control_allowed=False,
        interpretation='PyTorch CPU preflight only; excludes BWAPI feature collection, masks, arbitration, dispatch, logging and native implementation.')
    output.write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('checkpoint','frozen_source','output'):
        parser.add_argument(name,type=Path)
    args=parser.parse_args()
    print(json.dumps(probe(args.checkpoint,args.frozen_source,args.output)['measurements'],indent=2))
