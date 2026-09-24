"""Summarize this local plan's evidence without granting model control.

Completed research runs are checked against frozen source and checkpoint step /
sample counts. This report records remaining gates; it never changes a bot mode.
"""
import json
from pathlib import Path
import time

import torch

from .arena import inspect, verify
from .schema import sha256


def completed_run(output, source):
    state=json.loads((output/'status.json').read_text())
    if state['stage']!='complete':
        return dict(status=state['stage'],worker_pid=state['pid'],progress=state)
    spec=json.loads((output/'run.json').read_text())
    for name,digest in spec['source_sha256'].items():
        if sha256(source/'training'/name)!=digest:
            raise ValueError(f'frozen source changed: {name}')
    report=json.loads((output/'report.json').read_text())
    checkpoint=torch.load(output/'resume.pt',map_location='cpu',weights_only=True)
    if 'presentation_counts' in report:
        if (checkpoint['step']!=report['steps']
                or checkpoint['presented']!=report['presentation_counts']
                or sum(checkpoint['presented'].values())!=report['window_presentations']):
            raise ValueError('checkpoint and reported exposure disagree')
    else:
        if checkpoint['step']!=spec['steps'] or int(checkpoint['presented'].sum())!=report['presentations']:
            raise ValueError('scoped checkpoint and reported exposure disagree')
    metrics={}
    if 'after' in report:
        for split in ('train','dev'):
            result=report['after'][split]
            row=result['free_running']
            metrics[split]={k:row.get(k) for k in ('slot_commands','full_signature_correct',
                'position_known','position_within_64px','position_median_error_px',
                'actor_correct','non_right_click_correct')}
            metrics[split]['combat_actor_kind']=result.get('combat_actor_kind')
            metrics[split]['actor_sets']=result['decoded_actor_sets']
    return dict(status='complete',report=str((output/'report.json').resolve()),
        report_sha256=sha256(output/'report.json'),checkpoint_sha256=sha256(output/'resume.pt'),
        run_sha256=sha256(output/'run.json'),source_verified=True,checkpoint_step=checkpoint['step'],
        capacity_pass=report.get('capacity_pass'),
        development=(report.get('development') if 'presentation_counts' in report else
            dict(passed=report.get('passed'),checks=report.get('checks',{}),
                 measured=report.get('development'))),
        development_pass=report.get('development',{}).get('passed') if 'steps' in report else report.get('passed'),
        metrics=metrics,promotion_eligible=False)


def review(repo):
    root=repo/'artifacts/replay-learning'
    baseline=[]
    for name in ('baseline-01','baseline-pvt-02'):
        run=repo/'build/strength-first-20260924'/name
        verify(run)
        result=inspect(run)
        baseline.append(dict(name=name,scheduled=result['scheduled'],reported=result['reported_games'],
            normal_games=len(result['structurally_valid']),wins=sum(r['won'] for r in result['structurally_valid']),
            excluded=result['excluded'],manifest_sha256=sha256(run/'manifest.json')))
    jobs={}
    for name,output,source in (
        ('group_capacity','whole-game-group-capacity-20260924','group-capacity-20260924'),
        ('group_development','whole-game-group-development-20260924','group-development-20260924-r2'),
        ('macro_commitment','macro-commitment-development-20260924','macro-commitment-20260924'),
        ('macro_commitment_early','macro-commitment-early-development-20260924','macro-commitment-early-20260924')):
        jobs[name]=completed_run(root/output,repo/'build'/source/'source')
    inventory=json.loads((root/'whole-game-evaluation-inventory-20260924.json').read_text())
    runtime=json.loads((root/'whole-game-group-runtime-preflight-cached-terrain-20260924.json').read_text())
    blockers={}
    for name,job in jobs.items():
        if job['status']!='complete':
            blockers[name]=dict(status=job['status'])
        elif job.get('development_pass') is False:
            development=job['development']
            checks=development.get('checks',development)
            blockers[name]=dict(status='failed_development',
                failed_checks=[k for k,v in checks.items() if v is False])
    return dict(schema='protodd-local-training-plan-review-v1',updated_unix=time.time(),
        baseline=baseline,jobs=jobs,plan_complete=False,promotion_eligible=False,
        bounded_research_runs_complete=all(job['status']=='complete' for job in jobs.values()),
        offline_blockers=blockers,
        live_control_allowed=False,selected_scope='persistent economy/production priorities',
        evaluation_inventory=dict(globally_unused=inventory['globally_unused'],
            reserved_command_games=sum(inventory['counts'].values()),payloads_opened=inventory['payloads_opened'],
            globally_fresh=inventory['globally_fresh']),
        runtime_preflight=dict(python_numeric_parity=runtime['frozen_model_numeric_parity'],
            measurements=runtime['measurements'],synthetic=True,full_callback=False),
        remaining_gates=dict(disjoint_command_confirmation='not run; development must pass first',
            learned_scope_execution='not implemented or validated; scoped learning probes failed',
            export_win32_parity='not run for this architecture',
            complete_callback_timing='not run; Python model preflight is insufficient',
            paired_candidate_games='not run; baseline games do not validate a candidate',
            strength_72_games='not run; upstream competence/execution gates required'),
        final_test_payloads_opened=False,
        note='Research completion is distinct from plan completion. Failed development blocks dependent stages; these files authorize no control.')


if __name__=='__main__':
    repo=Path(__file__).resolve().parents[1]
    result=review(repo)
    output=repo/'build/strength-first-20260924/training-plan-status.json'
    temporary=output.with_suffix('.tmp.json')
    temporary.write_text(json.dumps(result,indent=2)+'\n')
    temporary.replace(output)
    print(json.dumps(dict(path=str(output),plan_complete=result['plan_complete'],
        jobs={k:v['status'] for k,v in result['jobs'].items()})))
