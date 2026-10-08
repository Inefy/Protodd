"""Version and fingerprint the live whole-game model contract."""
from __future__ import annotations

import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
WEIGHTS_SCHEMA = "protodd-whole-game-multislot-weights-v2"
ENCODER_SCHEMA = "protodd-whole-game-pilot-v3.2"
ACTION_SCHEMA = "protodd-whole-game-actions-v1"
ENCODER_SOURCES = (
    "training/whole_game_contract.py",
    "training/whole_game_features.py",
    "training/whole_game_model.py",
    "include/protodd/WholeGameObservation.hpp",
    "src/cpu/WholeGameEncoder.hpp",
    "src/cpu/WholeGameEncoder.cpp",
    "src/cpu/WholeGameCpu.hpp",
    "src/cpu/WholeGameCpu.cpp",
)
ACTION_SOURCES = (
    "training/whole_game_contract.py",
    "training/whole_game_action_schema.py",
    "training/whole_game_model.py",
    "training/whole_game_multislot_model.py",
    "src/cpu/WholeGameIntent.hpp",
    "src/cpu/WholeGameIntent.cpp",
    "src/bwapi/WholeGameAction.hpp",
    "src/bwapi/WholeGameAction.cpp",
)


def fingerprint(paths: tuple[str, ...], root: Path = ROOT) -> str:
    combined = hashlib.sha256()
    for relative in paths:
        path = root / relative
        combined.update(relative.encode("utf8") + b"\0")
        combined.update(hashlib.sha256(path.read_bytes()).digest())
    return combined.hexdigest()


def model_contract(root: Path = ROOT, weights_schema: str = WEIGHTS_SCHEMA) -> dict:
    observation_header = (root / "include/protodd/WholeGameObservation.hpp").read_text(encoding="utf8")
    action_header = (root / "src/cpu/WholeGameIntent.hpp").read_text(encoding="utf8")
    python_action_source = (root / "training/whole_game_action_schema.py").read_text(encoding="utf8")

    if (f'inline constexpr auto schema = "{ENCODER_SCHEMA}";' not in observation_header or
            f'ACTION_SCHEMA = "{ACTION_SCHEMA}"' not in python_action_source or
            f'inline constexpr auto actionSchema = "{ACTION_SCHEMA}";' not in action_header):
        raise ValueError("Python and Win32 whole-game encoder/decoder schemas disagree")
    return {
        "schema": "protodd-whole-game-model-contract-v1",
        "weights_schema": weights_schema,
        "encoder_schema": ENCODER_SCHEMA,
        "encoder_fingerprint": fingerprint(ENCODER_SOURCES, root),
        "action_schema": ACTION_SCHEMA,
        "action_fingerprint": fingerprint(ACTION_SOURCES, root),
    }
