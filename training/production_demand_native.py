"""Export and verify the frozen scoped model in a standalone Win32 process.

This measures model inference only, not a complete BWAPI callback or game gate.
"""
import argparse
import json
from pathlib import Path
import struct
import subprocess

import numpy as np
import torch

from .macro_commitment_probe import write
from .production_demand_fit import DemandModel
from .schema import load_schema,sha256


def run(args):
    if args.output.exists():raise FileExistsError(args.output)
    report=json.loads((args.confirmation/'report.json').read_text())
    if not report['gate']['passed']:raise ValueError('confirmation must pass')
    checkpoint_sha=sha256(args.fit/'resume.pt')
    if checkpoint_sha!=report['checkpoint_sha256']:raise ValueError('confirmed checkpoint changed')
    fit_spec=json.loads((args.fit/'run.json').read_text())
    if sha256(Path(__file__).with_name('production_demand_fit.py'))!=fit_spec['source_sha256']['production_demand_fit.py']:
        raise ValueError('model decoder changed')
    data_path=args.confirmation/'confirmation.npz'
    if sha256(data_path)!=json.loads((args.confirmation/'data.json').read_text())['confirmation']:
        raise ValueError('confirmation tensors changed')
    args.output.mkdir(parents=True)
    repo=Path(__file__).resolve().parents[1]
    native_files=[repo/'tools/production_demand_cpu/main.cpp',repo/'tools/production_demand_cpu/CMakeLists.txt']
    native_files += [repo/'include/protodd'/name for name in ('ProductionDemandModel.hpp','ObservationEncoder.hpp',
        'GameState.hpp','Geometry.hpp','UnitCatalog.hpp','Technology.hpp')]
    native_files += [repo/'src/core'/name for name in ('ProductionDemandModel.cpp','ObservationEncoder.cpp',
        'GameState.cpp','UnitCatalog.cpp','Technology.cpp')]
    spec=dict(schema='protodd-production-native-parity-v1',checkpoint_sha256=checkpoint_sha,
        confirmation_report_sha256=sha256(args.confirmation/'report.json'),
        executable_sha256=sha256(args.executable),input_sha256=sha256(data_path),
        absolute_logit_tolerance=.0002,required_count_agreement=1.0,
        source_sha256=sha256(__file__),
        native_source_sha256={str(p.relative_to(repo)):sha256(p) for p in native_files},
        promotion_eligible=False,full_callback_timing=False,live_control_allowed=False)
    write(args.output/'run.json',spec)
    torch.set_num_threads(2)
    saved=torch.load(args.fit/'resume.pt',map_location='cpu',weights_only=True)['model']
    schema=load_schema();inputs=len(saved['mean']);width=fit_spec['width']
    path=args.output/'ProductionDemand.bin'
    with path.open('wb') as stream:
        stream.write(struct.pack('<8sIQIIII',b'PTDDEM1\n',1,int(schema['fingerprint'],16),inputs,width,8,4))
        for name in ('mean','scale','net.0.weight','net.0.bias','net.2.weight','net.2.bias','net.4.weight','net.4.bias'):
            value=saved[name].detach().numpy().astype('<f4')
            if not np.isfinite(value).all():raise ValueError('invalid weights')
            stream.write(value.tobytes())
    with np.load(data_path,allow_pickle=False) as archive:features=archive['features'].copy()
    model=DemandModel(saved['mean'],saved['scale'],width).eval();model.load_state_dict(saved)
    with torch.no_grad():
        tensor=torch.from_numpy(features)
        expected=model(tensor).reshape(-1,32).numpy()
        gpu=model.cuda()(tensor.cuda()).cpu().reshape(-1,32).numpy()
    input_path=args.output/'inputs.bin'
    with input_path.open('wb') as stream:
        stream.write(struct.pack('<II',len(features),inputs));stream.write(features.astype('<f4').tobytes())
    output_path=args.output/'outputs.bin'
    completed=subprocess.run([str(args.executable.resolve()),str(path.resolve()),
        str(input_path.resolve()),str(output_path.resolve())],check=True,text=True,capture_output=True,timeout=60)
    timing=json.loads(completed.stdout)
    actual=np.fromfile(output_path,dtype='<f4').reshape(len(features),40)
    counts=(expected.reshape(-1,8,4)>=0).sum(-1)
    gpu_counts=(gpu.reshape(-1,8,4)>=0).sum(-1)
    maximum_error=float(np.max(np.abs(actual[:,:32]-expected)))
    gpu_error=float(np.max(np.abs(gpu-expected)))
    checks=dict(win32=timing['pointer_bits']==32,logits=maximum_error<=.0002,
        exact_counts=bool((actual[:,32:]==counts).all()),
        cpu_cuda_counts=bool((counts==gpu_counts).all()),cpu_cuda_logits=gpu_error<=.0002,
        invalid_inputs=timing['invalid_input_checks'])
    result=dict(checks=checks,passed=all(checks.values()),maximum_logit_error=maximum_error,
        cpu_cuda_maximum_logit_error=gpu_error,model_only_timing=timing,
        weights_sha256=sha256(path),input_file_sha256=sha256(input_path),output_file_sha256=sha256(output_path),
        executable_sha256=sha256(args.executable),promotion_eligible=False,full_callback_timing=False,
        live_control_allowed=False,note='Standalone inference only. No BWAPI feature/history adapter or dispatch is measured.')
    write(args.output/'report.json',result)
    print(json.dumps(result),flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    for name in ('fit','confirmation','executable','output'):parser.add_argument(name,type=Path)
    args=parser.parse_args();run(args)
