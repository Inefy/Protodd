"""Reconcile the two preserved baseline result files without inferring tactical causes."""
import json
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SERVER = ROOT / 'ladder/runs/cog-2026/tournament/server'
OUT = ROOT.parents[2] / 'outputs'
rows = [json.loads(line) for name in ('results_100.txt', 'results_100_retry.txt')
        for line in (SERVER / name).read_text().splitlines() if line.strip()]
games = defaultdict(list)
for row in rows:
    games[row['gameID']].append(row)
assert set(games) == set(range(100))
assert all(len(g) == 2 and len({r['reportingBot'] for r in g}) == 2 for g in games.values())
lines = ['# Protodd baseline audit', '',
         '100 scheduled games: 32 reported wins, 67 losses, one stalled game. '
         'Thirty wins are against proxy opponents with very low final scores and require opponent-health validation.', '',
         '| Opponent | Wins | Losses | Stalled | Opponent score range |',
         '|---|---:|---:|---:|---:|']
for bot in sorted({r['opponentBot'] for r in rows if r['reportingBot'] == 'Protodd'}):
    ours = [r for r in rows if r['reportingBot'] == 'Protodd' and r['opponentBot'] == bot]
    scores = [r['score'] for r in rows if r['reportingBot'] == bot]
    lines.append(f"| {bot} | {sum(r['won'] for r in ours)} | "
                 f"{sum(not r['won'] and not r['crash'] for r in ours)} | "
                 f"{sum(r['crash'] for r in ours)} | {min(scores)}–{max(scores)} |")
lines += ['', '## What the surviving evidence supports', '',
    '- Venator: final game used DTRushArbiter. Protodd saw an undetected Dark Templar at frame 7632, '
    'lost its army at 8712 and first Nexus at 9336. Its first Observer appeared around 9335 in the opponent log. '
    'At frame 7680, mobile-breakout goals explicitly suppressed Forge and Cannon construction despite cloak danger. '
    'Late detection and suppression of emergency static detection are concrete fix targets.',
    '- BananaBrain: last surviving opponent record identifies PvP_2gatedtexpo against Protodd P_4gategoon. '
    'The detailed Protodd traces for these games were overwritten; exact combat causes require new archived tests.',
    '- McRave: last surviving opponent record identifies a 1GateCore/ZCore/4Gate win over Protodd Robo. '
    'One separate game (27) stalled on both sides at frame 29160 and must not be counted as a strategic defeat.',
    '- Microwave: last surviving opponent history identifies 10Hatch9Pool9gas; its final game log records sustained '
    'Mutalisk/Zergling production. Missing Protodd traces prevent attributing all nine losses to one cause.',
    '- Steamhammer: last surviving opponent record identifies OverhatchLateGas and multiple expansions. '
    'Fresh Protodd telemetry is needed to distinguish economic disadvantage from combat/anti-air failures.',
    '- Stardust: its final log classifies Protodd as NoZealotCore before MidGame. No surviving Protodd trace '
    'supports a precise cause for every loss.',
    '- Pluto: all ten losses are confirmed; the surviving bandit data is insufficient for tactical attribution.',
    '- InfestedArtosis, PurpleWave, UAlbertaBot: all three are proxy bots, with 30/30 reported wins for Protodd. '
    'Their activity must be verified before including those wins in a strength benchmark.', '',
    'The earlier claim that all losses generally came from army/base collapse exceeded the preserved evidence. '
    'The previous detailed-results text includes only the resumed 94 games. The accompanying reconciled JSONL '
    'contains both reports for all 100 games. New experiments must archive each game separately.', '']
OUT.mkdir(parents=True, exist_ok=True)
(OUT / 'protodd_baseline_audit.md').write_text('\n'.join(lines), encoding='utf-8')
(OUT / 'protodd_baseline_all100.jsonl').write_text(
    '\n'.join(json.dumps(r) for r in sorted(rows, key=lambda r: (r['gameID'], r['reportingBot']))) + '\n', encoding='utf-8')
print('\n'.join(lines))
