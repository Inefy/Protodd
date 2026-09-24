"""Inventory prior model metadata and reserve command evaluation games.

Reads JSON metadata only. Never reads trajectory, tensor, replay or test payloads.
The earlier macro validation exposure is retained, not renamed as fresh data.
"""
import argparse
from collections import Counter
import hashlib
import json
from pathlib import Path
import re

from .schema import sha256


def ids_in(value):
    return set(re.findall(r'game:[0-9a-f]{64}', json.dumps(value)))


def inventory(root, release, output, per_matchup=8):
    if output.exists():
        raise FileExistsError(output)
    identity_path = release / 'identity.json'
    identity = json.loads(identity_path.read_text())
    manifest_path = root / 'protoss-data-release-20260922/manifest.json'
    manifest = json.loads(manifest_path.read_text())
    records = {g['game_id']: g for g in manifest['games']}
    used, sources = set(), {}
    # All metadata for older whole-game model/diagnostic runs, including window
    # selections. Extraction/quality receipts alone do not imply model exposure.
    paths = [p for p in root.glob('whole-game*/*.json')
             if not any(s in p.parent.name for s in ('release-', 'quality', 'pilot-',
                       'actions-', 'normalized-', 'shared-'))]
    paths += [p for p in root.glob('whole-game*.json') if 'quality' not in p.name]
    for p in sorted(set(paths)):
        if p.resolve() == output.resolve():
            continue
        d = json.loads(p.read_text(encoding='utf-8-sig'))
        found = ids_in(d)
        used.update(found)
        sources[str(p.resolve())] = dict(sha256=sha256(p), mentioned_games=len(found))
    # Conservatively exclude every validation game in earlier small extraction
    # cohorts even when its evaluation run did not persist explicit IDs.
    for p in sorted(root.glob('whole-game-release*/identity.json')):
        d = json.loads(p.read_text())
        if len(d['selected']) < len(identity['selected']):
            found = {g['game_id'] for g in d['selected'] if g['split'] == 'validation'}
            used.update(found)
            sources[str(p.resolve())] = dict(sha256=sha256(p), mentioned_games=len(found))
    aliases = set()
    for game in used:
        if game in records:
            aliases.update((records[game]['duplicate_group'], records[game]['replay_sha256']))
    used.update(g['game_id'] for g in records.values()
                if g['duplicate_group'] in aliases or g['replay_sha256'] in aliases)
    tensor_path = root / 'protoss-tensors-20260922/manifest.json'
    history_path = root / 'protoss-a0-gpu-20260922/history.json'
    tensor = json.loads(tensor_path.read_text())
    history = json.loads(history_path.read_text())
    validation_rows = tensor['counts']['validation']
    if not any(e['validation']['all']['samples'] == validation_rows for e in history):
        raise ValueError('macro history does not establish complete validation exposure')
    macro_used = {g['game_id'] for g in tensor['games'] if g['split'] == 'validation'}
    validation = [g for g in identity['selected'] if g['split'] == 'validation']
    command_unused = [g for g in validation if g['game_id'] not in used]
    globally_unused = [g for g in command_unused if g['game_id'] not in macro_used]
    selected = []
    for matchup in ('PvP', 'PvT', 'PvZ'):
        choices = [g for g in command_unused if g['matchup'] == matchup]
        choices.sort(key=lambda g: hashlib.sha256(('command-confirmation-20260924:' + g['game_id']).encode()).hexdigest())
        if len(choices) < per_matchup:
            raise ValueError('insufficient command-unused validation games')
        selected.extend(choices[:per_matchup])
    selected_aliases = [records[g['game_id']]['duplicate_group'] for g in selected]
    if len(set(selected_aliases)) != len(selected):
        raise ValueError('selected games include alternate recordings')
    report = dict(schema='protodd-evaluation-inventory-v1', purpose='prospective command confirmation',
        release_identity_sha256=sha256(identity_path), source_manifest_sha256=sha256(manifest_path),
        tensor_manifest_sha256=sha256(tensor_path), macro_history_sha256=sha256(history_path),
        source_sha256=sha256(Path(__file__)), prior_metadata=sources,
        prior_whole_game_ids=sorted(used), validation_total=len(validation),
        whole_game_command_unused=len(command_unused), globally_unused=len(globally_unused),
        macro_exposed_validation=len(macro_used), selected=selected,
        counts=dict(Counter(g['matchup'] for g in selected)), payloads_opened=False,
        final_test_payloads_opened=False, globally_fresh=False,
        requirement='Freeze candidate and criteria before opening this command cohort. It was used by macro-model validation and is not a globally untouched holdout. New games are required for a globally fresh replay claim.')
    output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'release', 'output'):
        parser.add_argument(name, type=Path)
    args = parser.parse_args()
    r = inventory(args.root, args.release, args.output)
    print(json.dumps({k:r[k] for k in ('validation_total','whole_game_command_unused','globally_unused','macro_exposed_validation','counts','globally_fresh')}))
