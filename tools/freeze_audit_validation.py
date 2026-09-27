"""Freeze the audit treatment and its counterfactual, with common instrumentation."""
from pathlib import Path
import shutil,json,hashlib
ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'build/audit-validation-20260926'
def digest(p):return hashlib.sha256(p.read_bytes()).hexdigest()
if __name__=='__main__':
    before=ROOT/'build/deep-audit-20260926/before'
    sources=[ROOT/'CMakeLists.txt']
    for directory in ('src','include','cmake','tests','tools'):
        sources += [p for p in (ROOT/directory).rglob('*') if p.is_file() and (p.suffix in {'.cpp','.hpp','.h','.cmake'} or p.name=='CMakeLists.txt')]
    for arm in ('candidate','reference'):
        base=OUT/f'source-{arm}'
        if base.exists():raise FileExistsError(base)
        for path in sources:
            rel=path.relative_to(ROOT); dest=base/rel;dest.parent.mkdir(parents=True,exist_ok=True)
            archived=before/rel.as_posix().replace('/','_')
            shutil.copy2(archived if arm=='reference' and archived.exists() else path,dest)
    source={arm:{p.relative_to(OUT/f'source-{arm}').as_posix():digest(p) for p in (OUT/f'source-{arm}').rglob('*') if p.is_file()} for arm in ('candidate','reference')}
    changed=[p for p in source['candidate'] if source['candidate'][p]!=source['reference'][p]]
    (OUT/'source-receipt.json').write_text(json.dumps({'sha256':source,'changed':changed,'inherited_both':['Reaver prerequisite fix','available composition development rule'],'promotion_allowed':False},indent=2))
    print(json.dumps({'changed':changed}))
