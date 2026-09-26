"""Freeze a local Tournament Manager campaign with paired, isolated game evidence.

Preparation never touches the shared StarCraft runtimes or starts a game. Training
and evaluation are separate immutable campaigns; a one-sided END is not a reward.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path
import re
import shutil
import zipfile

from .schema import sha256


# The manager rewrites its HTML result dashboard during each campaign. These
# files are outputs even when present in the copied template.
MUTABLE_OUTPUT_PREFIX = "server/html/results/"


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def prepare(template, output, dll, opponents, maps, purpose="development", race="Protoss", rounds=2, port=1347,
            server_jar=None, client_bundle=None, whole_game_observe=False, production_shadow=None, frame_limit=None,
            production_control_receipt=None, production_screen_receipt=None, worker_training_intervention=None):
    template, output, dll = map(lambda p: Path(p).resolve(), (template, output, dll))
    if purpose not in ("training", "development", "final-test"):
        raise ValueError("unknown campaign purpose")
    if race not in ("Protoss", "Terran", "Zerg") or type(rounds) is not int or rounds < 2 or rounds % 2:
        raise ValueError("race and an even number of rounds >=2 are required")
    if whole_game_observe and race != "Protoss":
        raise ValueError("whole-game live pilot currently supports Protoss only")
    if output.exists() or not dll.is_file() or not 1024 <= port <= 65535:
        raise ValueError("existing output, missing DLL or invalid port")
    if production_shadow and (race != 'Protoss' or not Path(production_shadow).is_file()):
        raise ValueError('production shadow needs Protoss and a model file')
    if worker_training_intervention is not None and (purpose != 'training' or race != 'Protoss' or
            worker_training_intervention not in ('baseline', 'plus-one', 'plus-two') or production_control_receipt):
        raise ValueError('worker interventions require isolated Protoss training without model command control')
    if frame_limit is not None and (type(frame_limit) is not int or frame_limit<240 or purpose=='final-test'):
        raise ValueError('bounded development/training frame limit required')
    if production_control_receipt:
        if purpose!='development' or not production_shadow or ((frame_limit is None or frame_limit>7200) and not production_screen_receipt):
            raise ValueError('production control is limited to bounded local development scenarios')
        receipt=json.loads(Path(production_control_receipt).read_text())
        if receipt.get('shadow_gate_pass') is not True or receipt.get('feedback_gate_pass') is not True:
            raise ValueError('production control requires passing shadow and real feedback gates')
        prior=Path(receipt['campaign']);verify(prior)
        if sha256(prior/'manifest.json')!=receipt['manifest_sha256'] or sha256(production_shadow)!=sha256(prior/'server/bots/Protodd/read/ProductionDemand.bin'):
            raise ValueError('production receipt/model binding differs')
        for game in receipt['games']:
            for name,digest in game['hashes'].items():
                if sha256(Path(game['directory'])/name)!=digest:raise ValueError('production evidence changed')
        if production_screen_receipt:
            screen=json.loads(Path(production_screen_receipt).read_text())
            if screen.get('passed') is not True or screen.get('schema')!='protodd-production-control-screen-v1':
                raise ValueError('full-game comparison requires passing controlled screen')
            for group in ('candidate','reference'):
                for row in screen[group]:
                    if sha256(row['log'])!=row['sha256']:raise ValueError('controlled screen evidence changed')
            candidate_campaign=Path(screen['live_audit']['campaign'])
            verify(candidate_campaign)
            if sha256(candidate_campaign/'server/bots/Protodd/read/ProductionDemand.bin')!=sha256(production_shadow):
                raise ValueError('screen model differs')
    settings = json.loads((template / "server/server_settings.json").read_text())
    available = {b["BotName"]: b for b in settings["bots"]}
    opponents = list(dict.fromkeys(opponents))
    maps = list(dict.fromkeys(maps))
    if not opponents or not maps or not set(opponents) <= set(available) or not set(maps) <= set(settings["maps"]):
        raise ValueError("unknown/empty opponents or maps")
    for opponent in opponents:
        if not re.fullmatch(r"[A-Za-z0-9_-]+", opponent):
            raise ValueError("invalid opponent name")
        if available[opponent]["BotType"] != "dll":
            raise ValueError("proxy opponents require separate launcher health validation")
        if not (template / "server/bots" / opponent / "AI" / (opponent + ".dll")).is_file():
            raise ValueError("missing opponent DLL")
    clients = []
    for choices in (("client1", "client-a"), ("client2", "client-b")):
        client = next((template / name for name in choices if (template / name / "client_settings.json").is_file()), None)
        if client is None or not zipfile.is_zipfile(client / "client.jar"):
            raise ValueError("missing or placeholder client JAR")
        cfg = json.loads((client / "client_settings.json").read_text())
        runtime = Path(cfg["ClientStarcraftDir"])
        if not (runtime / "StarCraft.exe").is_file():
            raise ValueError("client StarCraft runtime is missing")
        clients.append((client, cfg))
    if Path(clients[0][1]["ClientStarcraftDir"]).resolve() == Path(clients[1][1]["ClientStarcraftDir"]).resolve():
        raise ValueError("two independent runtimes required")
    server_jar = Path(server_jar).resolve() if server_jar else template / "server/server.jar"
    if not zipfile.is_zipfile(server_jar):
        raise ValueError("invalid server JAR")
    if client_bundle is not None:
        client_bundle = Path(client_bundle).resolve()
        if (not zipfile.is_zipfile(client_bundle / "client.jar") or
                not all((client_bundle / name).is_file() for name in ('stop-owned-starcraft.ps1', 'start-owned-starcraft.ps1'))):
            raise ValueError("invalid local client bundle")
    with zipfile.ZipFile(template / "server/required" / settings["mapsFile"]) as archive:
        if not set(maps) <= set(archive.namelist()):
            raise ValueError("map archive does not contain selected maps")
    bot = {"Protoss": "Protodd", "Terran": "TerranTodd", "Zerg": "ZergTodd"}[race]
    if bot in opponents:
        raise ValueError("opponent name conflicts with candidate")
    # All cheap validation precedes destination creation. A failed copy leaves an
    # explicitly unfinished directory; it never receives a launchable manifest.
    server = output / "server"
    server.mkdir(parents=True)
    for directory in ("required", "html"):
        shutil.copytree(template / "server" / directory, server / directory)
    shutil.copy2(server_jar, server / "server.jar")
    for name in opponents:
        target = server / "bots" / name
        shutil.copytree(template / "server/bots" / name / "AI", target / "AI")
        initial_read = template / "server/bots" / name / "read"
        if initial_read.exists():
            shutil.copytree(initial_read, target / "read")
        else:
            (target / "read").mkdir()
        (target / "write").mkdir()
    target = server / "bots" / bot
    for name in ("AI", "read", "write"):
        (target / name).mkdir(parents=True)
    shutil.copy2(dll, target / "AI" / (bot + ".dll"))
    (target / "read/Protodd-learning-mode.txt").write_text("validated-train\n" if purpose == "training" and worker_training_intervention is None else "frozen\n")
    (target / "read/Policy-mode.txt").write_text("train\n" if purpose == "training" and worker_training_intervention is None else "frozen\n")
    if worker_training_intervention is not None:
        (target / "read/WorkerTraining-mode.txt").write_text(worker_training_intervention+'\n')
    (target / "read/LearnedMacro-mode.txt").write_text("off\n")
    (target / "read/ProductionDemand-mode.txt").write_text('local-train-units\n' if production_control_receipt else 'shadow\n' if production_shadow else 'off\n')
    if production_shadow:
        shutil.copy2(production_shadow,target/'read/ProductionDemand.bin')
    if production_control_receipt:
        shutil.copy2(production_control_receipt,target/'read/ProductionDemand-evaluation-receipt.json')
    if production_screen_receipt:
        shutil.copy2(production_screen_receipt,target/'read/ProductionDemand-screen-receipt.json')
    if whole_game_observe:
        (target / "read/WholeGame-observe.txt").write_text("observe\n")
    settings["bots"] = [dict(BotName=bot, Race=race, BotType="dll", BWAPIVersion="BWAPI_440")] + [available[n] for n in opponents]
    settings.update(clearResults="no", gamesListFile="games.jsonl", resultsFile="results.jsonl",
                    maps=maps, serverPort=port, enableBotFileIO=False, lobbyGameSpeed="Fastest")
    settings["tournamentModuleSettings"]["frameSkip"] = 256
    settings["tournamentModuleSettings"]["localSpeed"] = 0
    if frame_limit is not None:settings['tournamentModuleSettings']['gameFrameLimit']=frame_limit
    write_json(server / "server_settings.json", settings)
    schedule = []
    for repetition in range(rounds):
        for map_name in maps:
            for opponent in opponents:
                home, away = (bot, opponent) if repetition % 2 == 0 else (opponent, bot)
                schedule.append(dict(gameID=len(schedule), roundID=repetition * len(maps) + maps.index(map_name),
                                     homeBot=home, awayBot=away, map=Path(map_name).name))
    (server / "games.jsonl").write_text("".join(json.dumps(g) + "\n" for g in schedule))
    for number, (client, cfg) in enumerate(clients, 1):
        destination = output / f"client{number}"
        destination.mkdir()
        shutil.copy2((client_bundle or client) / "client.jar", destination / "client.jar")
        if client_bundle:
            for helper in ('start-owned-starcraft.ps1', 'stop-owned-starcraft.ps1'):
                shutil.copy2(client_bundle / helper, destination / helper)
        cfg["ServerAddress"] = f"127.0.0.1:{port}"
        write_json(destination / "client_settings.json", cfg)
    hashes = {p.relative_to(output).as_posix(): sha256(p) for p in output.rglob("*")
              if p.is_file() and not p.relative_to(output).as_posix().startswith(MUTABLE_OUTPUT_PREFIX)}
    manifest = dict(format="protodd-arena-v1", complete=True, label=output.name, bot=bot, race=race,
                    purpose=purpose, games=len(schedule), template=str(template), components=hashes,
                    reward_requires="two consistent normal healthy reports plus reviewed opponent activity",
                    pairing="same maps/opponents with both host sides; actual seeds must be verified",
                    strength_validated=False)
    write_json(output / "manifest.json", manifest)
    return manifest


def verify(run):
    run = Path(run).resolve()
    manifest = json.loads((run / "manifest.json").read_text())
    if manifest.get("format") != "protodd-arena-v1" or manifest.get("complete") is not True:
        raise ValueError("unfinished or incompatible campaign")
    for name, digest in manifest["components"].items():
        # Older campaign manifests included dashboard files before they were
        # identified as manager-owned outputs. Keep their input audit usable.
        if name.startswith(MUTABLE_OUTPUT_PREFIX):
            continue
        path = (run / name).resolve()
        if not path.is_relative_to(run) or sha256(path) != digest:
            raise ValueError(f"campaign artifact changed: {name}")
    return dict(verified=True, games=manifest["games"], purpose=manifest["purpose"])


def inspect(run):
    """Classify paired evidence without converting a timeout or crash into a win."""
    from tools.train_openings import validate_pair
    run = Path(run)
    manifest = json.loads((run / "manifest.json").read_text())
    settings = json.loads((run / "server/server_settings.json").read_text())
    schedule = {g["gameID"]: g for g in map(json.loads, (run / "server/games.jsonl").read_text().splitlines())}
    groups = defaultdict(list)
    path = run / "server/results.jsonl"
    if path.exists():
        raw = path.read_text(encoding="utf-8")
        lines = raw.splitlines()
        for index, line in enumerate(lines):
            try:
                row = json.loads(line)
            except json.JSONDecodeError:
                if index == len(lines) - 1 and not raw.endswith("\n"):
                    break  # writer may still be appending
                raise
            groups[row["gameID"]].append(row)
    valid, excluded = [], []
    for gid, pair in sorted(groups.items()):
        try:
            game = schedule.get(gid)
            if game is None or any({r.get("reportingBot"), r.get("opponentBot")} != {game["homeBot"], game["awayBot"]}
                                   or r.get("map") != game["map"] for r in pair):
                raise ValueError("report does not match frozen schedule")
            own = validate_pair(pair, settings["tournamentModuleSettings"]["timeoutLimits"], manifest["bot"])
            valid.append(dict(game_id=gid, won=own["won"], opponent=own["opponentBot"], frame=own["finalFrame"]))
        except ValueError as error:
            excluded.append(dict(game_id=gid, reason=str(error)))
    return dict(purpose=manifest["purpose"], scheduled=manifest["games"], reported_games=len(groups),
                structurally_valid=valid, excluded=excluded, training_ready=False,
                opponent_activity_review="required before using outcomes as rewards", strength_validated=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    prepare_parser = commands.add_parser("prepare")
    for name in ("template", "output", "dll"):
        prepare_parser.add_argument("--" + name, type=Path, required=True)
    for name in ("opponents", "maps"):
        prepare_parser.add_argument("--" + name, nargs="+", required=True)
    prepare_parser.add_argument("--purpose", choices=["training", "development", "final-test"], default="development")
    prepare_parser.add_argument("--race", choices=["Protoss", "Terran", "Zerg"], default="Protoss")
    prepare_parser.add_argument("--rounds", type=int, default=2)
    prepare_parser.add_argument("--port", type=int, default=1347)
    prepare_parser.add_argument("--server-jar", type=Path, help="Explicitly rebuilt manager, pinned in the campaign")
    prepare_parser.add_argument("--client-bundle", type=Path, help="Rebuilt local client JAR and owned-process cleanup helper")
    prepare_parser.add_argument("--whole-game-observe", action="store_true", help="Record legal live observations from the opt-in BWAPI pilot")
    prepare_parser.add_argument('--production-shadow', type=Path, help='Frozen production-demand weights; shadow only')
    prepare_parser.add_argument('--frame-limit',type=int,help='Bounded development/training scenario; not strength evidence')
    prepare_parser.add_argument('--production-control-receipt',type=Path,help='Passing shadow/feedback receipt for bounded local train-unit scenarios')
    prepare_parser.add_argument('--production-screen-receipt',type=Path,help='Passing bounded control screen for a full-game local comparison')
    prepare_parser.add_argument('--worker-training-intervention',choices=('baseline','plus-one','plus-two'),help='Fixed local worker spending intervention, training episodes only')
    inspect_parser = commands.add_parser("inspect")
    inspect_parser.add_argument("run", type=Path)
    verify_parser = commands.add_parser("verify")
    verify_parser.add_argument("run", type=Path)
    args = vars(parser.parse_args())
    command = args.pop("command")
    action = {"prepare": prepare, "inspect": inspect, "verify": verify}[command]
    print(json.dumps(action(**args), indent=2))


if __name__ == "__main__":
    main()
