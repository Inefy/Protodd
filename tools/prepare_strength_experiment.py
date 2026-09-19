"""Prepare isolated matched tests; never overwrite the original tournament."""
import argparse
import hashlib
import json
import shutil
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('label')
parser.add_argument('dll', type=Path)
parser.add_argument('--proxy-health', action='store_true')
parser.add_argument('--resume-from', type=Path)
parser.add_argument('--learning-policy', type=Path)
parser.add_argument('--opponents', nargs='+')
parser.add_argument('--bot-name', choices=['Protodd', 'TerranTodd', 'ZergTodd'], default='Protodd')
parser.add_argument('--race', choices=['Protoss', 'Terran', 'Zerg'], default='Protoss')
parser.add_argument('--policy-mode', choices=['train', 'frozen'])
parser.add_argument('--q-policy', type=Path)
parser.add_argument('--maps', nargs='+')
parser.add_argument('--game-id-offset', type=int, default=0)
args = parser.parse_args()
if not 0 <= args.game_id_offset <= 1000000:
    raise SystemExit('Invalid game ID offset')
if {'Protodd': 'Protoss', 'TerranTodd': 'Terran', 'ZergTodd': 'Zerg'}[args.bot_name] != args.race:
    raise SystemExit('Bot alias and race must match')
if not args.label.replace('-', '').replace('_', '').isalnum():
    raise SystemExit('Invalid label')
source = root / 'ladder/runs/cog-2026/tournament'
destination = root / 'build/strength-20260918' / args.label
if destination.exists():
    raise SystemExit(f'Refusing to overwrite {destination}')
dll = args.dll.resolve()
if not dll.is_file():
    raise SystemExit('Missing frozen DLL')
server = destination / 'server'
server.mkdir(parents=True)
for name in ['required', 'html']:
    shutil.copytree(source / 'server' / name, server / name)
shutil.copy2(source / 'server/server.jar', server / 'server.jar')
settings = json.loads((source / 'server/server_settings.json').read_text())
opponents = ['Venator', 'BananaBrain', 'Stardust', 'Pluto', 'McRave', 'Microwave', 'Steamhammer']
if args.proxy_health:
    opponents = ['InfestedArtosis', 'PurpleWave', 'UAlbertaBot']
if args.opponents:
    if not set(args.opponents).issubset(opponents):
        raise SystemExit('Unknown opponent selection')
    opponents = list(dict.fromkeys(args.opponents))
settings['bots'] = [b for b in settings['bots'] if b['BotName'] in ['Protodd'] + opponents]
for bot in settings['bots']:
    name = bot['BotName']
    if name == 'Protodd':
        bot['BotName'] = args.bot_name
        bot['Race'] = args.race
        name = args.bot_name
    target = server / 'bots' / name
    if name == args.bot_name:
        (target / 'AI').mkdir(parents=True)
    else:
        shutil.copytree(source / 'server/bots' / name / 'AI', target / 'AI')
    (target / 'read').mkdir()
    (target / 'write').mkdir()
shutil.copy2(dll, server / f'bots/{args.bot_name}/AI/{args.bot_name}.dll')
if args.learning_policy:
    mode = (args.learning_policy / 'Protodd-learning-mode.txt').read_text().strip()
    if mode not in ('validated-train', 'frozen'):
        raise SystemExit('Policy must explicitly select controlled training or frozen evaluation')
    shutil.copytree(args.learning_policy, server / f'bots/{args.bot_name}/read', dirs_exist_ok=True)
if args.policy_mode:
    (server / f'bots/{args.bot_name}/read/Policy-mode.txt').write_text(args.policy_mode + '\n')
if args.q_policy:
    if not args.policy_mode:
        raise SystemExit('--q-policy requires --policy-mode')
    shutil.copy2(args.q_policy, server / f'bots/{args.bot_name}/read/Policy.q')
settings.update(clearResults='no', gamesListFile='games.jsonl', resultsFile='results.jsonl',
                serverPort=1347, enableBotFileIO=False, lobbyGameSpeed='Fastest')
settings['tournamentModuleSettings']['frameSkip'] = 64
(server / 'server_settings.json').write_text(json.dumps(settings, indent=2))
maps = ['(4)Clay Fields1p2(n).scm', '(2)Crossing_Field_1.34.scx']
if args.maps:
    available = {Path(m).name for m in settings['maps']}
    if not set(args.maps).issubset(available):
        raise SystemExit('Map must belong to configured tournament map pool')
    maps = args.maps
if args.proxy_health:
    maps = maps[:1]
schedule = []
for repeat, map_name in enumerate(maps):
    for opponent in opponents:
        pair = [args.bot_name, opponent] if repeat == 0 else [opponent, args.bot_name]
        schedule.append(dict(gameID=args.game_id_offset + len(schedule), roundID=repeat,
                             homeBot=pair[0], awayBot=pair[1], map=map_name))
if args.resume_from:
    previous = {}
    for raw in (args.resume_from / 'server/results.jsonl').read_text().splitlines():
        row = json.loads(raw)
        previous.setdefault(row['gameID'], {})[row['reportingBot']] = row
    complete = {gid for gid, pair in previous.items() if len(pair) == 2}
    schedule = [game for game in schedule if game['gameID'] not in complete]
(server / 'games.jsonl').write_text(''.join(json.dumps(g) + '\n' for g in schedule))
for i in (1, 2):
    client = destination / f'client{i}'
    client.mkdir()
    shutil.copy2(source / 'client/client.jar', client / 'client.jar')
    config = json.loads((source / f'client{i}/client_settings.json').read_text())
    config['ServerAddress'] = '127.0.0.1:1347'
    (client / 'client_settings.json').write_text(json.dumps(config, indent=2))
hashes = {str(p.relative_to(server)): hashlib.sha256(p.read_bytes()).hexdigest()
          for p in (server / 'bots').rglob('*') if p.is_file()}
manifest = dict(label=args.label, dll=str(dll), games=len(schedule), requestedSeedBase=180918,
                bot=args.bot_name, race=args.race, policyMode=args.policy_mode,
                pairedConditions='same opponent packages, maps, host sides and requested seeds; fresh learning',
                components=hashes)
if args.resume_from:
    manifest['resumes'] = str(args.resume_from.resolve())
if args.learning_policy:
    manifest['learningPolicy'] = str(args.learning_policy.resolve())
    manifest['learningMode'] = mode
    manifest['pairedConditions'] = 'fixed reviewed policy snapshot; no within-run outcome updates'
(destination / 'manifest.json').write_text(json.dumps(manifest, indent=2))
print(destination)
