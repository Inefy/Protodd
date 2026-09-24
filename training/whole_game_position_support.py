"""Position-class support in an already frozen train/development selection.

Reads verified label/terrain shards, never final-test data. This diagnoses
sample coverage and does not score, alter or select model checkpoints.
"""
from collections import Counter, defaultdict
import gzip
import json
from pathlib import Path

from .schema import sha256
from .whole_game_shards import selected_shards, load_terrain


def audit(release, fit, output):
    if output.exists():
        raise FileExistsError(output)
    selection=json.loads((fit/'selection.json').read_text())
    samples={name:defaultdict(dict) for name in ('train','dev')}
    for name in samples:
        for key in selection[name]:
            gid,frame,sequence=key.rsplit(':',2)
            samples[name][gid][int(frame)]=int(sequence)
    groups={gid:name for name in samples for gid in samples[name]}
    if len(groups)!=sum(len(g) for g in samples.values()):
        raise ValueError('overlapping games')
    positions=defaultdict(list)
    receipts={}
    command_count=Counter()
    for record,directory,receipt in selected_shards(release,'train',game_ids=set(groups)):
        gid=record['game_id'];name=groups[gid]
        receipts[gid]=sha256(directory/'receipt.json')
        terrain=load_terrain(directory)
        width,height=terrain['width_walktiles']*8,terrain['height_walktiles']*8
        windows=defaultdict(list)
        with gzip.open(directory/'imitation-labels.jsonl.gz','rt',encoding='utf-8') as stream:
            for line in stream:
                label=json.loads(line)
                frame=label['frame']//24*24
                if frame in samples[name][gid]:
                    windows[frame].append(label)
        for labels in windows.values():
            labels.sort(key=lambda x:x['observation_sequence'])
            for label in labels[:6]:
                command_count[name]+=1
                if label['loss_masks']['target_position']:
                    factor=32 if label['coordinate_space']=='build_tile' else 1
                    x,y=label['actions']['target_position']
                    xy=(min(.999999,max(0,x*factor/width)),min(.999999,max(0,y*factor/height)))
                    cell=(int(xy[0]*32),int(xy[1]*32))
                    positions[name].append(dict(cell=cell,xy=xy,kind=label['actions']['kind'],matchup=record['matchup']))
    if set(receipts)!=set(groups):
        raise ValueError('missing verified game')
    # Match the existing audited selection's denominators before interpreting
    # this label-only shortcut; any cadence/ordering difference is an error.
    before=json.loads((fit/'before.json').read_text())
    for name in ('train','dev'):
        score=before[name]['free_running']
        if len(positions[name])!=score['position_known'] or command_count[name]!=score['slot_commands']:
            raise ValueError('label-only support differs from causal audit selection')
    cells=Counter(tuple(p['cell']) for p in positions['train'])
    kinds=defaultdict(set)
    for p in positions['train']:
        kinds[p['kind']].add(tuple(p['cell']))
    dev=positions['dev']
    report=dict(schema='protodd-position-support-v1',selection_sha256=sha256(fit/'selection.json'),
        before_sha256=sha256(fit/'before.json'),release_identity_sha256=sha256(release/'identity.json'),
        source_sha256=sha256(Path(__file__)),receipts=receipts,
        train_position_labels=len(positions['train']),train_distinct_coarse_cells=len(cells),
        total_coarse_classes=1024,train_singleton_classes=sum(n==1 for n in cells.values()),
        dev_position_labels=len(dev),dev_cells_unseen_in_train=sum(tuple(p['cell']) not in cells for p in dev),
        dev_kind_cell_pairs_unseen=sum(tuple(p['cell']) not in kinds[p['kind']] for p in dev),
        by_kind={k:dict(samples=sum(p['kind']==k for p in dev),
            unseen_cells=sum(p['kind']==k and tuple(p['cell']) not in cells for p in dev),
            unseen_kind_cell_pairs=sum(p['kind']==k and tuple(p['cell']) not in kinds[k] for p in dev))
            for k in sorted({p['kind'] for p in dev})},
        no_model_fit=True,final_test_opened=False,
        interpretation='Coverage diagnosis only. A softmax can choose an unseen class, so this is not an accuracy ceiling or proof of the failure cause.')
    output.write_text(json.dumps(report,indent=2)+'\n')
    return report


if __name__=='__main__':
    import argparse
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('release','fit','output'):
        parser.add_argument(name,type=Path)
    args=parser.parse_args()
    r=audit(args.release,args.fit,args.output)
    print(json.dumps({k:v for k,v in r.items() if k not in ('receipts','by_kind')},indent=2))
