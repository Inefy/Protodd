"""Pinned local terrain overlay for Remastered replay playback; no game data ships."""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import struct
from pathlib import Path

PACK_SHA256 = "32f8cc3561e11d2756a579dc54675e0758e94c15b9f767a66a1ba533a9856a44"
TILESETS = ("badlands", "platform", "jungle", "desert", "ice", "twilight")
EXPECTED = {f"tileset/{name}.{ext}": size for name in TILESETS
            for ext, size in (("cv5", 52), ("vf4", 32))}


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def parse_pack(data: bytes) -> dict[str, bytes]:
    if data.startswith(b"\x1f\x8b"):
        data = gzip.decompress(data)
    if len(data) < 4:
        raise ValueError("Truncated asset pack")
    size, = struct.unpack_from("<I", data)
    offset = 4 + size
    if offset > len(data):
        raise ValueError("Truncated asset manifest")
    entries = json.loads(data[4:offset])
    if not isinstance(entries, list):
        raise ValueError("Invalid asset manifest")
    result = {}
    for entry in entries:
        name, length = entry["name"], entry["len"]
        if (not isinstance(name, str) or name in result or type(length) is not int
                or length < 0 or offset + length > len(data)):
            raise ValueError("Invalid or duplicate asset entry")
        result[name] = data[offset:offset + length]
        offset += length
    if offset != len(data):
        raise ValueError("Trailing asset pack bytes")
    return result


def prepare(pack_path: Path, output: Path) -> dict:
    packed = pack_path.read_bytes()
    if digest(packed) != PACK_SHA256:
        raise ValueError("Asset pack differs from the validated Remastered asset pin")
    assets = parse_pack(packed)
    # Validate the complete set before creating output. No classic fallback.
    selected = {}
    for name, record_size in EXPECTED.items():
        data = assets.get(name.replace("tileset/", "tilesets/"), b"")
        if not data or len(data) % record_size:
            raise ValueError(f"Missing or invalid modern terrain: {name}")
        selected[name] = data
    output.mkdir(parents=True, exist_ok=False)
    manifest = {"version": 1, "pack_sha256": PACK_SHA256, "files": {}}
    for name, data in selected.items():
        target = output / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        manifest["files"][name] = {"sha256": digest(data), "bytes": len(data)}
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return manifest


def verify(output: Path) -> dict:
    manifest = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    if (manifest.get("version") != 1 or manifest.get("pack_sha256") != PACK_SHA256
            or set(manifest.get("files", {})) != set(EXPECTED)):
        raise ValueError("Unexpected terrain asset manifest")
    for name, record_size in EXPECTED.items():
        data = (output / name).read_bytes()
        entry = manifest["files"][name]
        if (not data or len(data) % record_size or len(data) != entry["bytes"]
                or digest(data) != entry["sha256"]):
            raise ValueError(f"Terrain asset changed or truncated: {name}")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pack", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    manifest = prepare(args.pack, args.output)
    print(f"Prepared {len(manifest['files'])} pinned terrain assets in {args.output}")


if __name__ == "__main__":
    main()
