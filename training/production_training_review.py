"""Verify current production-learning evidence without granting control."""
import io
import json
from pathlib import Path
import time
import unittest

import torch

from .macro_commitment_probe import write
from .production_demand_fit import gates
from .schema import sha256


def source_check(spec,source):
    for name,digest in spec['source_sha256'].items():
        if sha256(source/'training'/name)!=digest:raise ValueError('frozen source changed: '+name)
    if sha256(source/'training/schema_v2.json')!=spec['schema_sha256']:
        raise ValueError('frozen schema changed')


def review(repo):
    root=repo/'artifacts/replay-learning';runs={}
    for name in ('capacity','development'):
        directory=root/f'production-demand-{name}-20260924'
        spec=json.loads((directory/'run.json').read_text())
        source_check(spec,repo/'build'/directory.name/'source')
        state=json.loads((directory/'status.json').read_text())
        if state['stage']!='complete':raise ValueError('production fit not complete')
        report=json.loads((directory/'report.json').read_text())
        checkpoint=torch.load(directory/'resume.pt',map_location='cpu',weights_only=True)
        if (checkpoint['step']!=spec['steps'] or checkpoint['step']!=report['steps'] or
                int(checkpoint['presented'].sum())!=report['presentations'] or
                int((checkpoint['presented']>0).sum())!=report['unique_gradient_rows'] or
                len(checkpoint['presented'])!=report['train_rows']):
            raise ValueError('training exposure mismatch')
        for split,digest in json.loads((directory/'data.json').read_text()).items():
            if sha256(directory/(split+'.npz'))!=digest:raise ValueError('training tensor cache changed')
        references=json.loads((directory/'reference.json').read_text())
        if gates(report['after']['dev'],references)!=report['development']:
            raise ValueError('development gate mismatch')
        runs[name]=dict(source_verified=True,checkpoint_sha256=sha256(directory/'resume.pt'),
            report_sha256=sha256(directory/'report.json'),steps=report['steps'],
            unique_rows=report['unique_gradient_rows'],presentations=report['presentations'],
            capacity_pass=report['capacity_pass'],development=report['development'])
    confirmation=root/'production-demand-confirmation-20260924'
    spec=json.loads((confirmation/'run.json').read_text())
    source_check(spec,repo/'build'/confirmation.name/'source')
    report=json.loads((confirmation/'report.json').read_text())
    if spec['checkpoint_sha256']!=runs['development']['checkpoint_sha256']:
        raise ValueError('confirmation checkpoint mismatch')
    if gates(report['confirmation'],report['references'])!=report['gate']:
        raise ValueError('confirmation gate mismatch')
    if sha256(confirmation/'confirmation.npz')!=json.loads((confirmation/'data.json').read_text())['confirmation']:
        raise ValueError('confirmation cache changed')
    native=root/'production-demand-native-20260924'
    native_spec=json.loads((native/'run.json').read_text())
    native_report=json.loads((native/'report.json').read_text())
    if native_spec['checkpoint_sha256']!=runs['development']['checkpoint_sha256']:
        raise ValueError('native checkpoint mismatch')
    for name,digest in native_spec['native_source_sha256'].items():
        if sha256(native/'source'/name)!=digest:raise ValueError('native source snapshot changed')
    for name,key in (('production_demand_cpu.exe','executable_sha256'),('ProductionDemand.bin','weights_sha256'),
                     ('inputs.bin','input_file_sha256'),('outputs.bin','output_file_sha256')):
        if sha256(native/name)!=native_report[key]:raise ValueError('native artifact changed')
    test_names=['tests.test_production_demands','tests.test_production_demand_fit',
                'tests.test_production_commitments','tests.test_training_atomic_status']
    output=io.StringIO();result=unittest.TextTestRunner(stream=output).run(unittest.defaultTestLoader.loadTestsFromNames(test_names))
    if not result.wasSuccessful():raise ValueError(output.getvalue())
    tests=dict(passed=True,count=result.testsRun,log=output.getvalue(),
        source_sha256={name:sha256(repo/(name.replace('.','/')+'.py')) for name in test_names},
        executor_sha256=sha256(repo/'training/production_commitments.py'))
    return dict(schema='protodd-production-training-review-v1',updated_unix=time.time(),fits=runs,
        confirmation=dict(passed=report['gate']['passed'],source_verified=True,
            report_sha256=sha256(confirmation/'report.json'),macro_f1=report['confirmation']['macro_f1'],
            count_mae=report['confirmation']['count_mae'],games=24,rows=report['confirmation']['rows'],
            globally_fresh=False,prior_macro_validation_exposure=True),
        native=dict(passed=native_report['passed'],report_sha256=sha256(native/'report.json'),
            maximum_logit_error=native_report['maximum_logit_error'],timing=native_report['model_only_timing']),
        offline_contract_tests=tests,
        remaining=['Causal live accepted-command history adapter and input parity',
            'Native persistent executor with real command/spend/completion feedback',
            'Full BWAPI callback timing and shadow scenarios',
            'Bounded controlled scenarios, paired gameplay and 72-game strength gate'],
        known_limit='Assimilator recall 7.35% in confirmation; offline gate pass does not authorize gas control',
        playing_strength_improved=False,plan_complete=False,promotion_eligible=False,live_control_allowed=False)


if __name__=='__main__':
    repo=Path(__file__).resolve().parents[1];result=review(repo)
    path=repo/'build/strength-first-20260924/production-training-status.json';write(path,result)
    print(json.dumps(dict(path=str(path),confirmation=result['confirmation'],native=result['native'],
        tests=result['offline_contract_tests']['count'],plan_complete=False)))
