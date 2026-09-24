"""Diagnose FP32 versus BF16 behavior on an already fitted capacity cohort."""
import argparse
from collections import defaultdict
import json
from pathlib import Path
from unittest.mock import patch

import torch

from .schema import sha256
from .whole_game_group_fit import balanced_subset, evaluate, sample_key
from .whole_game_group_model import GroupCommandModel
from .whole_game_group_batch import multislot_minibatch_loss
from .whole_game_group_batch import conditioned_action_loss
from .whole_game_model import masked_action_loss
from .whole_game_structured_loss import balanced_actor_loss
from .whole_game_group_model import position_classes
from torch.nn import functional as F
from .whole_game_multislot_collect import collect_multislot_windows
from .whole_game_encoding_cache import ObservationCache


def audit(fit, release, output):
    if output.exists():
        raise FileExistsError(output)
    torch.set_num_threads(2)
    spec=json.loads((fit/'run.json').read_text())
    buckets,_=collect_multislot_windows(release,'train',spec['train_games'],
        history=8,per_game_category_limit=64,category_limit=64,seed=spec['seed'])
    samples=balanced_subset(buckets,spec['windows_per_matchup'],spec['seed'])
    selection=json.loads((fit/'selection.json').read_text())
    if [sample_key(s) for s in samples]!=selection['train']:
        raise ValueError('frozen train selection differs')
    saved=torch.load(fit/'resume.pt',map_location='cpu',weights_only=True)
    model=GroupCommandModel(width=spec['width']).cuda().eval()
    model.load_state_dict(saved['model'])
    cache=ObservationCache(256*1024*1024)
    fp32=evaluate(model,samples,'cuda',cache)
    with torch.autocast(device_type='cuda',dtype=torch.bfloat16):
        bf16=evaluate(model,samples,'cuda',cache)
    recorded=json.loads((fit/'report.json').read_text())['after']['train']['free_running']
    for name in ('full_signature_correct','position_within_64px','kind_correct','actor_correct'):
        if fp32['free_running'][name]!=recorded[name]:
            raise ValueError('FP32 parity with recorded capacity audit failed: '+name)
    # Compare the objective with fixed weights as well as command counts. Batch
    # reduction weights actions differently from a mean over individual windows.
    # No optimizer is created and this never changes the fitted checkpoint.
    shapes=defaultdict(list)
    for sample in samples:
        shapes[tuple(sample[3].shape)].append(sample)
    objectives={}
    for name,training,probability,batch_size in (
        ('single_teacher',False,1.0,1), ('batch_teacher',False,1.0,spec['batch_size']),
        ('batch_generated',True,0.0,spec['batch_size']),
        ('batch_scheduled',True,0.5,spec['batch_size'])):
        model.train(training)
        model.teacher_probability=probability
        torch.manual_seed(spec['seed'])
        rows=[]
        with torch.no_grad():
            for bucket in shapes.values():
                for start in range(0,len(bucket),batch_size):
                    part=bucket[start:start+batch_size]
                    loss,details=multislot_minibatch_loss(model,part,'cuda',cache)
                    rows.append(dict(loss=float(loss),windows=len(part),
                        keys=[sample_key(s) for s in part],
                        presentations=[saved['presented'][sample_key(s)] for s in part],**details))
        objectives[name]=dict(mean_loss=sum(r['loss']*r['windows'] for r in rows)/len(samples),
            mean_stop=sum(r['stop']*r['windows'] for r in rows)/len(samples),
            mean_action=sum(r['action']*r['windows'] for r in rows)/len(samples),
            mean_delay=sum(r['delay']*r['windows'] for r in rows)/len(samples),
            maximum_loss=max(r['loss'] for r in rows),batches=rows)
    worst=max(objectives['single_teacher']['batches'],key=lambda row:row['loss'])
    sample=next(s for s in samples if sample_key(s)==worst['keys'][0])
    components=[]
    def explain(output,label):
        masked=dict(label,mask=dict(label['mask']))
        has_position=masked['mask'].pop('target_position',False)
        _,parts=masked_action_loss(output,masked)
        if masked['mask'].get('actors'):
            parts['actors']=balanced_actor_loss(output['actor'],label['actor_known'],label['actor_positive'])
            parts['actor_count']=F.cross_entropy(output['actor_count'],label['actor_positive'].sum(-1)-1)
        if has_position:
            cell,sub=position_classes(label['target_position'])
            parts['position_cell']=F.cross_entropy(output['position_cell'],cell)
            parts['position_subcell']=F.cross_entropy(output['position_subcell'],sub)
        # Compatibility penalty is already included in the unmodified objective.
        result=conditioned_action_loss(output,label)
        components.append(dict(heads={key:float(value) for key,value in parts.items()},
            total=float(result),known_actors=int(label['actor_known'].sum()),
            positive_actors=int(label['actor_positive'].sum())))
        return result
    model.eval()
    with torch.no_grad(),patch('training.whole_game_group_batch.conditioned_action_loss',explain):
        multislot_minibatch_loss(model,[sample],'cuda',cache)
    result=dict(schema='protodd-group-precision-audit-v1',checkpoint_sha256=sha256(fit/'resume.pt'),
        selection_sha256=sha256(fit/'selection.json'),
        sources={p.name:sha256(p) for p in Path(__file__).parent.glob('*.py')},
        fp32=fp32,bf16=bf16,recorded_fp32_reproduced=True,train_windows=len(samples),
        fixed_weight_objectives=objectives,
        worst_training_window=dict(**worst,components=components,
            labels=sample[2]['labels']),
        final_test_opened=False,no_model_fit=True,promotion_eligible=False,
        interpretation='Same fixed training windows, weights and teacher/free decoding; precision comparison only.')
    output.write_text(json.dumps(result,indent=2)+'\n')
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('fit','release','output'):
        parser.add_argument(name,type=Path)
    args=parser.parse_args()
    report=audit(args.fit,args.release,args.output)
    print(json.dumps({mode:dict(loss=report[mode]['mean_teacher_loss'],
        signatures=report[mode]['free_running']['full_signature_correct'],
        positions=report[mode]['free_running']['position_within_64px']) for mode in ('fp32','bf16')},indent=2))
    print(json.dumps({name:{k:v for k,v in values.items() if k!='batches'}
        for name,values in report['fixed_weight_objectives'].items()},indent=2))
