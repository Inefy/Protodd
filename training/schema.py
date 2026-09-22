"""The named schema is exported by model_tool from the C++ encoder."""
import hashlib
import json
import struct
from pathlib import Path

SCHEMA_PATH = Path(__file__).with_name("schema_v2.json")


def load_schema():
    schema = json.loads(SCHEMA_PATH.read_text(encoding="utf-8"))
    data = bytearray(schema["version"].encode() + b"\0")
    for feature in schema["features"]:
        data.extend(feature["name"].encode() + b"\0")
        data.extend(struct.pack("<f", feature["scale"]))
    for action in schema["actions"]:
        data.extend(action.encode() + b"\0")
    value = 14695981039346656037
    for byte in data:
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    if f"{value:x}" != schema["fingerprint"]:
        raise ValueError("schema fingerprint mismatch; regenerate with model_tool schema")
    if len(set(f["name"] for f in schema["features"])) != len(schema["features"]):
        raise ValueError("duplicate feature names")
    return schema


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for block in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
