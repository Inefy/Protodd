"""Stage pinned Pluto and stock BananaBrain for local opening comparisons."""
import argparse
import json
from pathlib import Path
import shutil
import sys
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from training.schema import sha256


PLUTO_RELEASE = 'https://github.com/tscmoo/pluto/releases/tag/cog2026-2578600'
PLUTO_ZIP_SHA = 'd4e2225446f5048e131065357173952f0286586da77ebb8bddf7fc766c3844a5'
PLUTO_FILES = {
    'pluto.dll': '7e360b643c8c0156c03fe0cad9972a3058138ccfe22f921c5b4e0cd0aaf0abef',
    'pluto/pluto_infer.exe': 'ad880d8be52a6ef03893fa20644b6627e04fcd55e030178c6d52486b82340f2b',
    'pluto/pluto_weights.bin': '00b400eace6e4782202ebdcb3c30db76054aaa6a08c6a7dcb59575abc2d0a26e',
}


def prepare(template, banana_ai, pluto_zip, output):
    template, banana_ai, pluto_zip, output = map(Path, (template, banana_ai, pluto_zip, output))
    if output.exists() or sha256(pluto_zip) != PLUTO_ZIP_SHA:
        raise ValueError('existing destination or unrecognized Pluto package')
    settings = json.loads((template / 'server/server_settings.json').read_text())
    output.mkdir(parents=True)
    for name in ('client1', 'client2', 'server/required', 'server/html'):
        shutil.copytree(template / name, output / name)
    shutil.copy2(template / 'server/server.jar', output / 'server/server.jar')
    bots = output / 'server/bots'
    shutil.copytree(banana_ai, bots / 'BananaBrain/AI')
    for name in ('BananaBrain', 'Pluto'):
        for directory in ('AI', 'read', 'write'):
            (bots / name / directory).mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(pluto_zip) as archive:
        for name, digest in PLUTO_FILES.items():
            # Never extract arbitrary archive paths or executable companions.
            target = bots / 'Pluto/AI' / ('Pluto.dll' if name == 'pluto.dll' else name)
            target.parent.mkdir(parents=True, exist_ok=True)
            with archive.open(name) as source, target.open('wb') as destination:
                shutil.copyfileobj(source, destination)
            if sha256(target) != digest:
                raise ValueError(f'Pluto component checksum mismatch: {name}')
    settings['bots'] = [dict(BotName=name, Race='Protoss', BotType='dll', BWAPIVersion='BWAPI_440')
                        for name in ('BananaBrain', 'Pluto')]
    (output / 'server/server_settings.json').write_text(json.dumps(settings, indent=2) + '\n')
    provenance = dict(pluto_release=PLUTO_RELEASE, pluto_zip_sha256=PLUTO_ZIP_SHA,
                      pluto_files=PLUTO_FILES, banana_ai_sha256={
                          p.relative_to(banana_ai).as_posix(): sha256(p)
                          for p in banana_ai.rglob('*') if p.is_file()},
                      opponent_race='Protoss', fresh_opponent_read=True)
    (output / 'opponents.json').write_text(json.dumps(provenance, indent=2) + '\n')
    return provenance


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('template', 'banana-ai', 'pluto-zip', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    print(json.dumps(prepare(**vars(parser.parse_args())), indent=2))
