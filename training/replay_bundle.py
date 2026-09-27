"""Bundle the human-replay source corpus for Git LFS distribution.

The release manifest is the authority for split membership. The default
restore mode extracts only the training split, even though the source archive
also preserves held-out and unassigned replays.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import time
import zipfile


SCHEMA = "protodd-human-replay-source-bundle-v1"


def digest(path: Path) -> str:
    sha = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            sha.update(chunk)
    return sha.hexdigest()


def safe_name(name: str) -> str:
    path = PurePosixPath(name)
    if (not name or path.is_absolute() or path.as_posix() != name or "\\" in name or
            any(part in ("", ".", "..") for part in path.parts) or
            len(path.parts) != 2 or not name.lower().endswith(".rep")):
        raise ValueError(f"unsafe replay path: {name!r}")
    return name


def release_members(manifest: dict) -> dict[str, dict]:
    members = {}
    for game in manifest["games"]:
        name = safe_name(game["path"])
        if name in members or (game["split"] == "train" and
                               (game.get("map_holdout") or game.get("player_holdout"))):
            raise ValueError(f"invalid release member: {name}")
        members[name] = game
    if not any(game["split"] == "train" for game in members.values()):
        raise ValueError("release manifest contains no training replays")
    return members


def archive_matches(path: Path, members: list[dict]) -> bool:
    if not path.is_file():
        return False
    try:
        with zipfile.ZipFile(path) as reader:
            if reader.namelist() != [game["path"] for game in members]:
                return False
            for game in members:
                sha = hashlib.sha256()
                with reader.open(game["path"]) as replay:
                    for chunk in iter(lambda: replay.read(1024 * 1024), b""):
                        sha.update(chunk)
                if sha.hexdigest() != game["sha256"]:
                    return False
    except (OSError, zipfile.BadZipFile):
        return False
    return True


def replace_when_released(source: Path, destination: Path) -> None:
    for attempt in range(40):
        try:
            os.replace(source, destination)
            return
        except PermissionError:
            if attempt == 39:
                raise
            time.sleep(0.25)


def create(manifest_path: Path, source_root: Path, output: Path) -> dict:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    release = release_members(manifest)
    sources = sorted(source_root.glob("*/*.rep"))
    if not sources:
        raise ValueError("source replay directory is empty")
    games = []
    for source in sources:
        name = safe_name(source.relative_to(source_root).as_posix())
        sha = digest(source)
        member = release.get(name)
        if member is not None and sha != member["replay_sha256"]:
            raise ValueError(f"release replay checksum mismatch: {source}")
        games.append({"path": name, "sha256": sha,
                      "split": member["split"] if member else "unassigned"})
    if not set(release).issubset(game["path"] for game in games):
        raise ValueError("a frozen release replay is missing from the source corpus")
    output.mkdir(parents=True, exist_ok=True)
    groups: dict[str, list[dict]] = {}
    for game in games:
        groups.setdefault(game["path"].split("/")[0], []).append(game)

    archives = []
    for matchup, members in sorted(groups.items()):
        archive = output / f"{matchup}.zip"
        temporary = output / f"{matchup}.zip.tmp"
        if not archive_matches(archive, members):
            with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED,
                                 allowZip64=True) as writer:
                for game in members:
                    source = source_root / Path(game["path"])
                    info = zipfile.ZipInfo(game["path"], (1980, 1, 1, 0, 0, 0))
                    info.compress_type = zipfile.ZIP_STORED
                    info.external_attr = 0o644 << 16
                    with source.open("rb") as replay, writer.open(info, "w") as target:
                        shutil.copyfileobj(replay, target, 1024 * 1024)
            replace_when_released(temporary, archive)
        archives.append({"file": archive.name, "matchup": matchup,
                         "games": len(members), "bytes": archive.stat().st_size,
                         "sha256": digest(archive)})

    split_games = {split: sum(game["split"] == split for game in games)
                   for split in ("train", "validation", "test", "unassigned")}
    bundle = {"schema": SCHEMA, "scope": "source", "source": "cwal.gg",
              "release_manifest_sha256": digest(manifest_path),
              "split_games": split_games, "games": games, "archives": archives}
    (output / "manifest.json").write_text(
        json.dumps(bundle, separators=(",", ":"), sort_keys=True) + "\n",
        encoding="utf-8")
    return bundle


def restore(bundle_root: Path, output: Path, scope: str = "train") -> dict:
    bundle = json.loads((bundle_root / "manifest.json").read_text(encoding="utf-8"))
    if (bundle.get("schema") != SCHEMA or bundle.get("scope") != "source" or
            scope not in ("train", "source")):
        raise ValueError("unsupported replay bundle")
    members = {safe_name(game["path"]): game for game in bundle["games"]}
    if len(members) != len(bundle["games"]):
        raise ValueError("duplicate replay path in bundle")
    restored = 0
    for record in bundle["archives"]:
        archive = bundle_root / record["file"]
        if digest(archive) != record["sha256"]:
            raise ValueError(f"archive checksum mismatch: {archive}")
        with zipfile.ZipFile(archive) as reader:
            names = reader.namelist()
            if len(names) != record["games"] or any(
                    safe_name(name) not in members or
                    name.split("/")[0] != record["matchup"] for name in names):
                raise ValueError(f"archive membership mismatch: {archive}")
            for name in names:
                if scope == "train" and members[name]["split"] != "train":
                    continue
                destination = output / Path(name)
                if destination.exists():
                    if digest(destination) != members[name]["sha256"]:
                        raise ValueError(f"existing replay checksum mismatch: {destination}")
                    continue
                destination.parent.mkdir(parents=True, exist_ok=True)
                temporary = destination.with_suffix(".rep.tmp")
                try:
                    with reader.open(name) as source, temporary.open("wb") as target:
                        shutil.copyfileobj(source, target, 1024 * 1024)
                    if digest(temporary) != members[name]["sha256"]:
                        raise ValueError(f"replay checksum mismatch: {name}")
                    os.replace(temporary, destination)
                finally:
                    temporary.unlink(missing_ok=True)
                restored += 1
    selected = sum(scope == "source" or game["split"] == "train"
                   for game in members.values())
    return {"games": selected, "restored": restored, "output": str(output)}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    pack = commands.add_parser("create")
    pack.add_argument("--manifest", type=Path, required=True)
    pack.add_argument("--source", type=Path, required=True)
    pack.add_argument("--output", type=Path, required=True)
    unpack = commands.add_parser("restore")
    unpack.add_argument("--bundle", type=Path, required=True)
    unpack.add_argument("--output", type=Path, required=True)
    unpack.add_argument("--scope", choices=("train", "source"), default="train")
    args = parser.parse_args()
    result = (create(args.manifest, args.source, args.output)
              if args.command == "create" else
              restore(args.bundle, args.output, args.scope))
    print(json.dumps({"games": len(result["games"]),
                      "split_games": result["split_games"],
                      "archives": result["archives"]} if args.command == "create"
                     else result, indent=2))


if __name__ == "__main__":
    main()
