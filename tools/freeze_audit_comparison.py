"""Seal the completed audit preflight and predeclare the live decision gates."""
import json,sys,hashlib,shutil
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from training.audit_validation_review import fixtures,timing
from training.schema import sha256
OUT=ROOT/'build/audit-validation-20260926'
if __name__=='__main__':
    destination=OUT/'comparison-plan.json'
    if destination.exists():raise FileExistsError(destination)
    engine={arm:fixtures(OUT,arm) for arm in ('reference','candidate')}
    assert all(v['passed'] for v in engine['candidate'].values()),engine
    assert all(not engine['reference'][case]['passed'] for case in ('storm-allies','producer','prerequisite','combat'))
    assert engine['reference']['storm-clear']['passed']
    assert all(engine['reference'][case]['map_sha256']==engine['candidate'][case]['map_sha256'] for case in engine['candidate'])
    stress={arm:timing(OUT/f'load-{arm}/full-callback-us.bin') for arm in ('candidate','reference')}
    assert all(t['max_us']<55000 and t['p99_us']<42000 and t['samples']>=1200 for t in stress.values())
    assert '100% tests passed, 0 tests failed out of 38' in (OUT/'native-tests-final.log').read_text(encoding='utf-8-sig')
    assert '100% tests passed, 0 tests failed out of 5' in (OUT/'win32-tests.log').read_text(encoding='utf-8-sig')
    sources={arm:{p.relative_to(OUT/f'source-{arm}').as_posix():sha256(p) for p in (OUT/f'source-{arm}').rglob('*') if p.is_file()} for arm in ('reference','candidate')}
    for path,digest in sources['candidate'].items():
        if path.startswith(('src/','include/')):assert sha256(ROOT/path)==digest,path
    receipt=json.loads((OUT/'source-receipt.json').read_text());receipt['sha256']=sources
    receipt['changed']=[p for p in sources['candidate'] if sources['candidate'][p]!=sources['reference'][p]]
    (OUT/'source-receipt-final.json').write_text(json.dumps(receipt,indent=2)+'\n')
    manifests={arm:json.loads((OUT/arm/'manifest.json').read_text()) for arm in ('reference-a','reference-b','candidate')}
    assert manifests['reference-a']['components']==manifests['reference-b']['components']
    changed=[k for k,v in manifests['candidate']['components'].items() if v!=manifests['reference-a']['components'][k]]
    assert changed==['server/bots/Protodd/AI/Protodd.dll'],changed
    pins={f'source-{arm}/{p}':digest for arm,items in sources.items() for p,digest in items.items()}
    for folder in ('scenarios-verified-candidate','scenarios-verified-reference','load-candidate','load-reference','client-bundle'):
        for path in (OUT/folder).rglob('*'):
            if path.is_file():pins[path.relative_to(OUT).as_posix()]=sha256(path)
    for name in ('Protodd.reference.dll','Protodd.candidate.dll','source-receipt-final.json','native-tests-final.log','win32-tests.log'):
        pins[name]=sha256(OUT/name)
    for arm in manifests:pins[f'{arm}/manifest.json']=sha256(OUT/arm/'manifest.json')
    dependencies=['training/audit_validation_review.py','training/pvz_approach_generalization_review.py',
        'training/pvz_core_transition_review.py','training/pvz_approach_grace_screen.py','training/arena.py','training/schema.py',
        'scripts/continue-audit-validation.ps1','scripts/start-arena.ps1','scripts/run-audit-scenarios.ps1','scripts/run-audit-load.ps1',
        'tools/audit_scenario_maps.py','tools/prepare_audit_comparison.py','tools/freeze_audit_comparison.py']
    plan=dict(schema='protodd-audit-comparison-plan-v1',games_per_arm=12,frame_limit=30000,seed_base=202609800,
        opponents=['McRaveZ','Steamhammer','BananaBrain'],maps=['Benzene','Destination'],
        reference_dll_sha256=sha256(OUT/'Protodd.reference.dll'),candidate_dll_sha256=sha256(OUT/'Protodd.candidate.dll'),
        source_changes=receipt['changed'],component_changes=changed,
        gates=dict(callback_p99_us=42000,callback_max_us=55000,max_matchup_win_drop=0,
            max_mean_army_drop=.5,max_mean_worker_drop=1.,max_mean_early_loss_increase=.5,
            max_mean_defender_delay=120,minimum_win_gain=2),
        sha256=pins,repo_sha256={p:sha256(ROOT/p) for p in dependencies},
        purpose='bounded development across three races with exact-DLL repeated references; no tournament promotion',
        strength_gate='Only a passed development screen advances to a separately frozen disjoint 72-game minimum strength gate. No capped outcomes are win labels.',
        promotion_allowed=False)
    destination.write_text(json.dumps(plan,indent=2)+'\n')
    print(json.dumps({'plan_sha256':sha256(destination),'pinned_files':len(pins),'engine_pass':True,'stress':stress}))
