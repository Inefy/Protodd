"""Verify this bounded population-goal cycle and publish its remaining gates."""
import json
from pathlib import Path
import time

import torch

from .macro_commitment_probe import write
from .schema import sha256


def review(repo):
    base = repo/'artifacts/replay-learning'
    snapshots = ('production-goal-source-20260924', 'production-worker-goal-source-20260924',
                 'production-worker-confirm-source-20260924')
    for name in snapshots:
        directory = repo/'build'/name
        for filename, digest in json.loads((directory/'source-sha256.json').read_text()).items():
            if sha256(directory/filename) != digest:
                raise ValueError('population-goal source snapshot changed')
    fits = {}
    for name in ('capacity', 'development'):
        path = base/('production-goal-'+name+'-20260924')
        spec = json.loads((path/'run.json').read_text())
        state = json.loads((path/'status.json').read_text())
        report = json.loads((path/'report.json').read_text())
        if state['stage'] != 'complete':
            raise ValueError('population-goal fit incomplete')
        checkpoint = torch.load(path/'resume.pt', map_location='cpu', weights_only=True)
        if (checkpoint['step'] != spec['steps'] or checkpoint['step'] != report['steps']
                or int(checkpoint['presented'].sum()) != report['presentations']
                or int((checkpoint['presented'] > 0).sum()) != report['unique_gradient_rows']):
            raise ValueError('population-goal checkpoint exposure mismatch')
        for split, digest in json.loads((path/'data.json').read_text()).items():
            if sha256(path/(split+'.npz')) != digest:
                raise ValueError('population-goal tensors changed')
        for filename, digest in spec['source_sha256'].items():
            if sha256(repo/'build'/snapshots[0]/'training'/filename) != digest:
                raise ValueError('fit source differs from snapshot')
        fits[name] = dict(report=str(path/'report.json'), report_sha256=sha256(path/'report.json'),
                          checkpoint_sha256=sha256(path/'resume.pt'), steps=checkpoint['step'],
                          presentations=report['presentations'], unique_gradient_rows=report['unique_gradient_rows'],
                          capacity_pass=report['capacity_pass'], development=report['development'])
    worker = base/'production-worker-goal-check-20260924'
    confirm = base/'production-worker-goal-confirmation-20260924'
    wr = json.loads((worker/'report.json').read_text())
    cr = json.loads((confirm/'report.json').read_text())
    frozen = json.loads((worker/'frozen-model.json').read_text())
    if not (sha256(worker/'worker-goal.npz') == wr['model_sha256'] == cr['model_sha256'] == frozen['model_sha256']):
        raise ValueError('worker-goal weights differ across checks')
    for path, snapshot in ((worker, snapshots[1]), (confirm, snapshots[2])):
        spec = json.loads((path/'run.json').read_text())
        if json.loads((path/'status.json').read_text())['stage'] != 'complete':
            raise ValueError('worker evaluation incomplete')
        for filename, digest in spec['source_sha256'].items():
            if sha256(repo/'build'/snapshot/'training'/filename) != digest:
                raise ValueError('worker source differs from snapshot')
        for split, digest in json.loads((path/'data.json').read_text()).items():
            if sha256(path/(split+'.npz')) != digest:
                raise ValueError('worker evaluation tensors changed')
    return dict(schema='protodd-population-goal-cycle-v1', updated_unix=time.time(),
                stage='complete_failed_confirmation' if not cr['confirmation']['passed'] else 'confirmation_passed',
                source_sha256=sha256(__file__), sources_verified=True, fits=fits,
                worker_holdout=dict(report=str(worker/'report.json'), sha256=sha256(worker/'report.json'),
                                    passed=wr['holdout']['passed'], metrics=wr['candidate']),
                worker_confirmation=dict(report=str(confirm/'report.json'), sha256=sha256(confirm/'report.json'),
                                         gate=cr['confirmation'], metrics=cr['candidate'], by_matchup=cr['by_matchup']),
                remaining=['Outcome-labelled training scenarios for worker spending and recovery; no repeat of rejected goal-head fits',
                           'New bounded learning/development pass, then fixed disjoint confirmation',
                           'Native export/parity and real persistent worker feedback qualification',
                           'Complete callback timing, controlled screens, paired play and 72-game strength gate'],
                final_test_opened=False, live_control_allowed=False, promotion_eligible=False, full_plan_complete=False)


if __name__ == '__main__':
    repo = Path(__file__).resolve().parents[1]
    result = review(repo)
    path = repo/'build/strength-first-20260924/production-goal-status.json'
    write(path, result)
    print(json.dumps(dict(path=str(path), stage=result['stage'], sources_verified=result['sources_verified'])))
