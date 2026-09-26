"""Consolidate immutable integration evidence and the owned local pilot queue."""
import json
from pathlib import Path
import time

from .arena import verify
from .macro_commitment_probe import write
from .schema import sha256


def review(repo):
    base=repo/'build/strength-first-20260924'
    feedback_path=base/'production-feedback-05-report.json'
    control_path=base/'production-control-06-screen.json'
    feedback=json.loads(feedback_path.read_text());control=json.loads(control_path.read_text())
    for report in (feedback,control['live_audit']):
        campaign=Path(report['campaign']);verify(campaign)
        if sha256(campaign/'manifest.json')!=report['manifest_sha256']:raise ValueError('integration manifest changed')
        for game in report['games']:
            for name,digest in game['hashes'].items():
                if sha256(Path(game['directory'])/name)!=digest:raise ValueError('integration evidence changed')
    queue_path=base/'production-paired-07.status.json'
    queue=json.loads(queue_path.read_text(encoding='utf-8-sig')) if queue_path.exists() else None
    paired_path=base/'production-paired-07-report.json'
    paired=json.loads(paired_path.read_text()) if paired_path.exists() else None
    if paired:
        for label in ('candidate','reference'):
            report=paired[label]['audit'];campaign=Path(report['campaign']);verify(campaign)
            if sha256(campaign/'manifest.json')!=paired[label]['manifest_sha256']:raise ValueError('paired manifest changed')
    return dict(schema='protodd-production-integration-status-v1',updated_unix=time.time(),
        source_sha256=sha256(__file__),feedback_report_sha256=sha256(feedback_path),
        control_screen_sha256=sha256(control_path),
        causal_history_and_live_model_parity_pass=feedback['shadow_gate_pass'],
        native_train_unit_feedback_pass=feedback['feedback_gate_pass'],bounded_control_screen_pass=control['passed'],
        bounded_control_exercised=True,controlled_scope=['train_probe','train_zealot','train_dragoon'],
        callback_p95_us=[g['callback']['p95_us'] for g in feedback['games']],
        paired_queue=queue,paired_pilot=({'path':str(paired_path),'sha256':sha256(paired_path),
            'wins':paired['wins'],'checks':paired['checks'],'advance_to_72':paired['advance_to_72']} if paired else None),
        remaining=(['72-game diverse strength gate','building ownership/cancellation before broader scope'] if paired and paired['advance_to_72'] else
            ['Learned policy improvement before another strength attempt','72-game strength gate','building ownership/cancellation before broader scope'] if paired else
            ['Complete seed-matched full-game pilot','72-game strength gate if pilot merits it','building ownership/cancellation before broader scope']),
        full_plan_complete=False,strength_proven=False,tournament_control_allowed=False,final_test_opened=False)

if __name__=='__main__':
    repo=Path(__file__).resolve().parents[1];r=review(repo)
    path=repo/'build/strength-first-20260924/production-integration-status.json';write(path,r)
    print(json.dumps(dict(path=str(path),queue=r['paired_queue'],pilot=r['paired_pilot'])))
