"""Summarise completed paired games and archived diagnostic evidence."""
import argparse
import json
from collections import defaultdict
from pathlib import Path

from log_analyzer import analyze
from train_openings import validate_pair

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('runs', nargs='+', type=Path)
parser.add_argument('--output', type=Path, required=True)
args = parser.parse_args()
lines = ['# Protodd improvement experiments', '',
         'Results below are completed observations. Small samples do not establish a reliable win-rate improvement.', '']
for run in args.runs:
    manifest = json.loads((run / 'manifest.json').read_text())
    bot = manifest.get('bot', 'Protodd')
    path = run / 'server/results.jsonl'
    reports = defaultdict(dict)
    if path.exists():
        for raw in path.read_text().splitlines():
            try:
                row = json.loads(raw)
            except json.JSONDecodeError:
                continue  # A writer may still be appending the last record.
            reports[row['gameID']][row['reportingBot']] = row
    completed = {gid: pair for gid, pair in reports.items() if len(pair) == 2}
    ours = [pair[bot] for pair in completed.values() if bot in pair]
    settings = json.loads((run / 'server/server_settings.json').read_text())
    valid = []
    excluded = []
    for row in ours:
        try:
            validate_pair(list(completed[row['gameID']].values()),
                          settings['tournamentModuleSettings']['timeoutLimits'], bot)
            valid.append(row)
        except ValueError as error:
            excluded.append(f"Game {row['gameID']} ({row['opponentBot']}): {error}")
    lines += [f"## {manifest['label']}", '',
              f"Completed {len(completed)}/{manifest['games']}; strategic-valid {len(valid)}; "
              f"wins {sum(r['won'] for r in valid)}; losses {sum(not r['won'] for r in valid)}.", '',
              '| Opponent | Strategic wins | Strategic losses |', '|---|---:|---:|']
    for opponent in sorted({r['opponentBot'] for r in valid}):
        group = [r for r in valid if r['opponentBot'] == opponent]
        lines.append(f"| {opponent} | {sum(r['won'] for r in group)} | {sum(not r['won'] for r in group)} |")
    lines += ['', 'Excluded from strategic totals: ' + ('; '.join(excluded) or 'none'), '',
              '### Preserved per-game diagnostic summaries', '',
              'Raw summaries below include excluded outcomes; their won flag is not validation.', '']
    archive = run / 'server/replays/bot-write'
    logs = sorted(archive.rglob('Protodd.log')) if archive.exists() else []
    for log in logs:
        try:
            result = analyze(log.read_text(encoding='utf-8', errors='replace').splitlines())
        except (ValueError, KeyError) as error:
            lines.append(f'- Parse error in {log.relative_to(run)}: {error}')
            continue
        for detail in result.get('game_details', []):
            summary = detail.get('summary', {})
            lines.append(f"- {log.relative_to(run)}: " + json.dumps(summary, sort_keys=True))
    if not logs:
        lines.append('No completed per-game trace has been archived yet.')
    lines += ['']
args.output.parent.mkdir(parents=True, exist_ok=True)
args.output.write_text('\n'.join(lines), encoding='utf-8')
print('\n'.join(lines))
