"""Generate and validate the build/runtime feature registry.

The registry is derived from CMake option declarations plus the runtime control
files read by the BWAPI module. Keep explicit release values, ownership, and
configuration rules here so build, package, and campaign manifests agree.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
REGISTRY_PATH = ROOT / "docs" / "feature-registry.json"
OPTION_PATTERN = re.compile(
    r'option\(\s*(PROTODD_[A-Z0-9_]+)\s+"([^"\n]+)"\s+(ON|OFF)\s*\)',
    re.MULTILINE,
)
CACHE_INPUT_PATTERN = re.compile(
    r'set\(\s*(PROTODD_[A-Z0-9_]+)\s+""\s+CACHE\s+(?:FILEPATH|PATH)\s+"([^"\n]+)"\s*\)',
    re.MULTILINE,
)

TOURNAMENT_OVERRIDES = {
    "PROTODD_BUILD_TESTS": "ON",
    "PROTODD_BUILD_BWAPI_MODULE": "ON",
    "PROTODD_TOURNAMENT_PROFILE": "ON",
    "PROTODD_DEVELOPER_PROFILE": "OFF",
    "PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY": "OFF",
    "PROTODD_STALLED_ARMY_ROUTING": "ON",
}

DEPENDENCIES = {
    "PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY": {"requires_all": ["PROTODD_DEVELOPER_PROFILE"]},
    "PROTODD_WHOLE_GAME_EVALUATION_BUILD": {"requires_all": ["PROTODD_WHOLE_GAME_CONTROL"]},
    "PROTODD_WHOLE_GAME_HYBRID": {
        "requires_all": ["PROTODD_WHOLE_GAME_CONTROL", "PROTODD_WHOLE_GAME_EVALUATION_BUILD"]
    },
    "PROTODD_PVZ_EARLY_MINERAL_FALLBACK": {
        "requires_all": ["PROTODD_PVZ_REPLAY_OPENING", "PROTODD_PVZ_MINERAL_FALLBACK"]
    },
    "PROTODD_PVZ_PROACTIVE_REAVER": {"requires_all": ["PROTODD_PVZ_POWERED_CANNON_SCREEN"]},
    "PROTODD_PVZ_ARMY_FLOOR": {"requires_all": ["PROTODD_PVZ_POWERED_CANNON_SCREEN"]},
    "PROTODD_PVZ_DETECTOR_SURGE": {"requires_all": ["PROTODD_PVZ_ARMY_FLOOR"]},
}

RUNTIME_CONTROLS = [
    {
        "id": "learned_macro_mode", "file": "LearnedMacro-mode.txt", "kind": "mode",
        "owner": "ModelRuntime", "description": "Optional replay-trained macro policy observation.",
        "allowed_values": ["unset", "off", "shadow"], "default_value": "off",
        "tournament_values": ["unset", "off"],
        "purpose_values": {"training": ["unset", "off", "shadow"],
                           "development": ["unset", "off", "shadow"],
                           "final-test": ["unset", "off"]},
        "dependencies": [], "baseline": "off", "evidence": ["src/bwapi/ModelRuntime.cpp"],
        "promotion_status": "shadow-only; no command authority",
    },
    {
        "id": "policy_mode", "file": "Policy-mode.txt", "kind": "mode",
        "owner": "PolicyRuntime", "description": "Offline-trained matchup policy selection mode.",
        "allowed_values": ["unset", "off", "train", "frozen"], "default_value": "off",
        "tournament_values": ["unset", "off", "frozen"],
        "purpose_values": {"training": ["unset", "off", "train", "frozen"],
                           "development": ["unset", "off", "frozen"],
                           "final-test": ["unset", "frozen"]},
        "dependencies": [], "baseline": "off", "evidence": ["src/bwapi/PolicyRuntime.cpp", "tests/test_policy_learning.cpp"],
        "promotion_status": "frozen only outside training",
    },
    {
        "id": "production_demand_mode", "file": "ProductionDemand-mode.txt", "kind": "mode",
        "owner": "ProductionRuntime", "description": "Production model off, shadow, or local train-unit control.",
        "allowed_values": ["unset", "off", "shadow", "local-train-units"], "default_value": "off",
        "tournament_values": ["unset", "off", "shadow"],
        "purpose_values": {"training": ["unset", "off", "shadow"],
                           "development": ["unset", "off", "shadow", "local-train-units"],
                           "final-test": ["unset", "off", "shadow"]},
        "dependencies": [], "dependencies_by_value": {"local-train-units": ["PROTODD_PRODUCTION_LOCAL_EVALUATION"]}, "baseline": "off",
        "evidence": ["src/bwapi/ProductionRuntime.cpp"],
        "promotion_status": "shadow-only in release; command control is development-gated",
    },
    {
        "id": "protodd_learning_mode", "file": "Protodd-learning-mode.txt", "kind": "mode",
        "owner": "ProtoddModule", "description": "Per-alias opening history update policy.",
        "allowed_values": ["unset", "frozen", "validated-train"], "default_value": "frozen",
        "tournament_values": ["unset", "frozen"],
        "purpose_values": {"training": ["unset", "frozen", "validated-train"],
                           "development": ["unset", "frozen"],
                           "final-test": ["unset", "frozen"]},
        "dependencies": [], "baseline": "frozen", "evidence": ["src/bwapi/ProtoddModule.cpp", "tests/test_main.cpp"],
        "promotion_status": "validated-train is training-only",
    },
    {
        "id": "worker_training_mode", "file": "WorkerTraining-mode.txt", "kind": "mode",
        "owner": "ProtoddModule", "description": "Local worker-production intervention.",
        "allowed_values": ["unset", "baseline", "plus-one", "plus-two"], "default_value": "unset",
        "tournament_values": ["unset"],
        "purpose_values": {"training": ["unset", "baseline", "plus-one", "plus-two"],
                           "development": ["unset"], "final-test": ["unset"]},
        "dependencies": [], "dependencies_by_value": {"baseline": ["PROTODD_PRODUCTION_LOCAL_EVALUATION"],
            "plus-one": ["PROTODD_PRODUCTION_LOCAL_EVALUATION"], "plus-two": ["PROTODD_PRODUCTION_LOCAL_EVALUATION"]}, "baseline": "unset",
        "evidence": ["src/bwapi/ProtoddModule.cpp", "training/arena.py", "tests/test_arena.py"],
        "promotion_status": "isolated training only",
    },
    {
        "id": "tactical_target_mode", "file": "TacticalTarget-mode.txt", "kind": "mode",
        "owner": "ProtoddModule", "description": "Replay-trained tactical target evaluation mode.",
        "allowed_values": ["unset", "local-target"], "default_value": "unset",
        "tournament_values": ["unset"],
        "purpose_values": {"training": ["unset"], "development": ["unset", "local-target"],
                           "final-test": ["unset"]},
        "dependencies": [], "dependencies_by_value": {"local-target": ["PROTODD_TACTICAL_LOCAL_EVALUATION"]}, "baseline": "unset",
        "evidence": ["src/bwapi/ProtoddModule.cpp", "tests/test_arena.py"],
        "promotion_status": "local evaluation only",
    },
    {
        "id": "callback_audit_mode", "file": "CallbackAudit-mode.txt", "kind": "mode",
        "owner": "ProtoddModule", "description": "Optional callback timing audit.",
        "allowed_values": ["unset", "on"], "default_value": "unset",
        "tournament_values": ["unset"],
        "purpose_values": {"training": ["unset", "on"], "development": ["unset", "on"],
                           "final-test": ["unset"]},
        "dependencies": [], "baseline": "unset", "evidence": ["src/bwapi/ProtoddModule.cpp"],
        "promotion_status": "diagnostic-only",
    },
    {
        "id": "allin_opening", "file": "AllIn-opening.txt", "kind": "mode",
        "owner": "ProtoddModule", "description": "Native replay-derived opening profile.",
        "allowed_values": ["unset", "auto", "standard", "two-gate-zealot", "three-gate-dragoon",
                           "four-gate-dragoon", "dt-pressure"], "default_value": "auto",
        "tournament_values": ["unset"],
        "purpose_values": {"training": ["unset"], "development": ["unset", "auto", "standard",
            "two-gate-zealot", "three-gate-dragoon", "four-gate-dragoon", "dt-pressure"],
            "final-test": ["unset"]},
        "dependencies": [], "dependencies_by_value": {value: ["PROTODD_NATIVE_ALLIN_OPENING"]
            for value in ("auto", "standard", "two-gate-zealot", "three-gate-dragoon", "four-gate-dragoon", "dt-pressure")},
        "baseline": "auto",
        "evidence": ["src/bwapi/ProtoddModule.cpp", "tests/test_arena.py"],
        "promotion_status": "development-only pending matchup qualification",
    },
    {
        "id": "whole_game_hybrid_mode", "file": "WholeGame-hybrid-mode.txt", "kind": "mode",
        "owner": "WholeGameRuntime", "description": "Shadow or target command arbitration mode.",
        "allowed_values": ["unset", "shadow", "target"], "default_value": "unset",
        "tournament_values": ["unset", "shadow"],
        "purpose_values": {"training": ["unset"], "development": ["unset", "shadow", "target"],
                           "final-test": ["unset"]},
        "dependencies": [], "dependencies_by_value": {"shadow": ["PROTODD_WHOLE_GAME_CONTROL", "PROTODD_WHOLE_GAME_EVALUATION_BUILD"],
            "target": ["PROTODD_WHOLE_GAME_CONTROL", "PROTODD_WHOLE_GAME_EVALUATION_BUILD"]},
        "baseline": "unset", "evidence": ["src/bwapi/ProtoddModule.cpp", "training/arena.py", "tests/test_arena.py"],
        "promotion_status": "target authority requires exact weight promotion receipt",
    },
    {
        "id": "whole_game_observe", "file": "WholeGame-observe.txt", "kind": "presence",
        "owner": "WholeGameRuntime", "description": "Emit optional live whole-game observations.",
        "allowed_values": ["absent", "present"], "default_value": "absent",
        "tournament_values": ["absent"],
        "purpose_values": {"training": ["absent", "present"], "development": ["absent", "present"],
                           "final-test": ["absent"]},
        "dependencies": [], "baseline": "absent", "evidence": ["src/bwapi/WholeGameRuntime.cpp", "training/arena.py"],
        "promotion_status": "diagnostic-only",
    },
]

RUNTIME_SCOPES = {
    "learned_macro_mode": "diagnostic shadow only; disabled in final-test campaigns",
    "policy_mode": "training or frozen policy evaluation",
    "production_demand_mode": "shadow observation; local command control only in development",
    "protodd_learning_mode": "validated opening learning only in training",
    "worker_training_mode": "isolated Protoss training only",
    "tactical_target_mode": "local Protoss development only",
    "callback_audit_mode": "training and development diagnostics",
    "allin_opening": "local Protoss development only",
    "whole_game_hybrid_mode": "local Protoss development only",
    "whole_game_observe": "training and development diagnostics",
}

CONFIGURATION_RULES = [
    {"id": "profiles-exclusive", "when": "PROTODD_TOURNAMENT_PROFILE",
     "requires_all": [], "requires_any": [], "conflicts": ["PROTODD_DEVELOPER_PROFILE"],
     "message": "Choose either the tournament profile or the developer profile",
     "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "tournament-research-disabled", "when": "PROTODD_TOURNAMENT_PROFILE",
     "requires_all": [], "requires_any": [], "conflicts": ["PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY"],
     "message": "The tournament profile cannot enable learned research authority",
     "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "research-needs-developer", "when": "PROTODD_WHOLE_GAME_RESEARCH_AUTHORITY",
     "requires_all": ["PROTODD_DEVELOPER_PROFILE"], "requires_any": [], "conflicts": [],
     "message": "Learned research authority requires the developer profile",
     "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "module-profile-required", "when": "PROTODD_BUILD_BWAPI_MODULE",
     "requires_all": [], "requires_any": ["PROTODD_TOURNAMENT_PROFILE", "PROTODD_DEVELOPER_PROFILE"],
     "conflicts": [], "message": "BWAPI modules require an explicit tournament or developer profile",
     "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "pvz-gateway-conflicts", "when": "PROTODD_PVZ_GATEWAY_OPENING",
     "requires_all": [], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_EARLY_SPLASH", "PROTODD_PVZ_REPLAY_OPENING", "PROTODD_PVZ_ARCHIVES_FIRST"],
     "message": "Unsupported combination of PvZ strategy interventions", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "pvz-splash-conflicts", "when": "PROTODD_PVZ_EARLY_SPLASH",
     "requires_all": [], "requires_any": [], "conflicts": ["PROTODD_PVZ_REPLAY_OPENING", "PROTODD_PVZ_ARCHIVES_FIRST"],
     "message": "Unsupported combination of PvZ strategy interventions", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "pvp-fog-isolated", "when": "PROTODD_PVP_FOG_DETECTION",
     "requires_all": [], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_GATEWAY_OPENING", "PROTODD_PVZ_EARLY_SPLASH",
                   "PROTODD_PVZ_REPLAY_OPENING", "PROTODD_PVZ_ARCHIVES_FIRST"],
     "message": "Evaluate one opt-in strategy intervention at a time", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "late-economy-isolated", "when": "PROTODD_LATE_ECONOMY_RECOVERY",
     "requires_all": [], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_GATEWAY_OPENING", "PROTODD_PVZ_EARLY_SPLASH", "PROTODD_PVP_FOG_DETECTION"],
     "message": "Unsupported combination with late economy recovery", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "cannon-screen-base", "when": "PROTODD_PVZ_POWERED_CANNON_SCREEN",
     "requires_all": ["PROTODD_PVZ_REPLAY_OPENING"], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_ARCHIVES_FIRST", "PROTODD_LATE_ECONOMY_RECOVERY",
                   "PROTODD_PVZ_GATEWAY_OPENING", "PROTODD_PVZ_EARLY_SPLASH",
                   "PROTODD_PVP_FOG_DETECTION", "PROTODD_STALLED_ARMY_ROUTING"],
     "message": "Powered Cannon screen requires the replay PvZ opening", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "early-mineral-prerequisites", "when": "PROTODD_PVZ_EARLY_MINERAL_FALLBACK",
     "requires_all": ["PROTODD_PVZ_REPLAY_OPENING", "PROTODD_PVZ_MINERAL_FALLBACK"],
     "requires_any": [], "conflicts": ["PROTODD_PVZ_POWERED_CANNON_SCREEN"],
     "message": "Early mineral fallback requires its base flag and replay PvZ opening", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "proactive-reaver-prerequisite", "when": "PROTODD_PVZ_PROACTIVE_REAVER",
     "requires_all": ["PROTODD_PVZ_POWERED_CANNON_SCREEN"], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_MINERAL_FALLBACK"],
     "message": "Proactive Reaver requires the isolated powered Cannon screen", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "army-floor-prerequisite", "when": "PROTODD_PVZ_ARMY_FLOOR",
     "requires_all": ["PROTODD_PVZ_POWERED_CANNON_SCREEN"], "requires_any": [],
     "conflicts": ["PROTODD_PVZ_MINERAL_FALLBACK", "PROTODD_PVZ_PROACTIVE_REAVER"],
     "message": "PvZ army floor requires the isolated powered Cannon screen", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "detector-surge-prerequisite", "when": "PROTODD_PVZ_DETECTOR_SURGE",
     "requires_all": ["PROTODD_PVZ_ARMY_FLOOR"], "requires_any": [], "conflicts": [],
     "message": "PvZ detector surge requires the frozen army-floor reference", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "whole-game-evaluation-needs-control", "when": "PROTODD_WHOLE_GAME_EVALUATION_BUILD",
     "requires_all": ["PROTODD_WHOLE_GAME_CONTROL"], "requires_any": [], "conflicts": [],
     "message": "Evaluation build requires whole-game control", "evidence": ["tests/test_build_profiles.ps1"]},
    {"id": "hybrid-needs-evaluation", "when": "PROTODD_WHOLE_GAME_HYBRID",
     "requires_all": ["PROTODD_WHOLE_GAME_CONTROL", "PROTODD_WHOLE_GAME_EVALUATION_BUILD"],
     "requires_any": [], "conflicts": [], "message": "Hybrid control requires a weighted local evaluation controller",
     "evidence": ["tests/test_build_profiles.ps1"]},
]


def _owner(name: str) -> str:
    if name.startswith("PROTODD_BUILD_") or name.endswith("_PROFILE") or "RESEARCH_AUTHORITY" in name:
        return "build and release configuration"
    if name.startswith("PROTODD_PVZ_") or name.startswith("PROTODD_PVP_"):
        return "StrategyEngine"
    if "DETECTOR" in name or "PRESSURE" in name or "RALLY" in name or "SCREEN" in name:
        return "SquadPlanner and tactical coordination"
    if "MINING" in name or "WORKER" in name or "ECONOMY" in name or "BASE_" in name:
        return "Workers and expansion planning"
    if "WHOLE_GAME" in name or "TACTICAL" in name or "PRODUCTION" in name or "ALLIN" in name:
        return "learned and experimental controllers"
    if "ROUTING" in name:
        return "OperationsManager"
    return "core gameplay"


def _scope(name: str) -> str:
    if name in {"PROTODD_BUILD_TESTS", "PROTODD_BUILD_BWAPI_MODULE"}:
        return "build configuration"
    if name.endswith("_PROFILE") or "RESEARCH_AUTHORITY" in name:
        return "release and developer profiles"
    if name == "PROTODD_STALLED_ARMY_ROUTING":
        return "production-default gameplay"
    if "EVALUATION" in name or "CONTROL" in name or "HYBRID" in name or "ALLIN" in name:
        return "local evaluation unless explicitly promoted"
    return "opt-in gameplay experiment"


def _reference_map(root: Path, names: set[str]) -> dict[str, dict[str, list[str]]]:
    refs = {name: {"source_refs": [], "test_refs": []} for name in names}
    files = []
    for directory in ("cmake", "include", "src", "tests", "scripts", "training", "tools"):
        base = root / directory
        if base.exists():
            files.extend(path for path in base.rglob("*")
                         if path.is_file() and path.suffix.lower() in {".cmake", ".cpp", ".hpp", ".h", ".ps1", ".py"})
    for path in files:
        try:
            content = path.read_text(encoding="utf-8", errors="ignore")
        except OSError:
            continue
        relative = path.relative_to(root).as_posix()
        bucket = "test_refs" if relative.startswith("tests/") else "source_refs"
        for name in names:
            if name in content:
                refs[name][bucket].append(relative)
    for buckets in refs.values():
        for bucket in buckets.values():
            bucket.sort()
    return refs


def generate_registry(root: Path = ROOT) -> dict:
    cmake = (root / "CMakeLists.txt").read_text(encoding="utf-8")
    options = []
    specs = OPTION_PATTERN.findall(cmake)
    references = _reference_map(root, {name for name, _, _ in specs})
    for name, description, default in specs:
        dependencies = DEPENDENCIES.get(name, {})
        evidence = {key: list(value) for key, value in references[name].items()}
        evidence["source_refs"] = sorted(set(["CMakeLists.txt", *evidence["source_refs"]]))
        options.append({
            "id": name,
            "kind": "cmake-option",
            "description": description,
            "default": default,
            "tournament_value": TOURNAMENT_OVERRIDES.get(name, default),
            "scope": _scope(name),
            "owner": _owner(name),
            "baseline": default,
            "dependencies": dependencies,
            "evidence": evidence,
            "promotion_status": (
                "infrastructure" if name.startswith("PROTODD_BUILD_") else
                "release-default" if name in TOURNAMENT_OVERRIDES else
                "unpromoted-opt-in"
            ),
        })
    inputs = []
    for name, description in CACHE_INPUT_PATTERN.findall(cmake):
        inputs.append({
            "id": name,
            "kind": "cmake-input",
            "description": description,
            "tournament_value": "",
            "scope": "build provenance; cleared for tournament builds",
            "owner": "build and release configuration",
            "baseline": "unset",
            "dependencies": [],
            "evidence": {"source_refs": ["CMakeLists.txt"], "test_refs": []},
            "promotion_status": "not-authority-by-itself",
        })
    return {
        "schema": "protodd-feature-registry-v1",
        "options": options,
        "build_inputs": inputs,
        "runtime_controls": [
            {**control, "scope": RUNTIME_SCOPES[control["id"]]}
            for control in RUNTIME_CONTROLS
        ],
        "configuration_rules": CONFIGURATION_RULES,
    }


def configuration_violations(enabled: set[str], registry: dict | None = None) -> list[str]:
    registry = registry or json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    errors = []
    for rule in registry["configuration_rules"]:
        if rule["when"] not in enabled:
            continue
        if any(name not in enabled for name in rule["requires_all"]):
            errors.append(rule["message"])
            continue
        if rule["requires_any"] and not any(name in enabled for name in rule["requires_any"]):
            errors.append(rule["message"])
            continue
        if any(name in enabled for name in rule["conflicts"]):
            errors.append(rule["message"])
    return errors


def validate_registry(root: Path = ROOT) -> list[str]:
    errors = []
    try:
        current = json.loads(REGISTRY_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return [f"cannot read registry: {exc}"]
    generated = generate_registry(root)
    if current.get("schema") != generated["schema"]:
        errors.append("unsupported feature registry schema")
    expected = {item["id"]: item for item in generated["options"]}
    actual = {item.get("id"): item for item in current.get("options", [])}
    if set(expected) != set(actual):
        errors.append(f"CMake option inventory differs: missing={sorted(set(expected)-set(actual))}, extra={sorted(set(actual)-set(expected))}")
    for name in sorted(set(expected) & set(actual)):
        for field in ("kind", "description", "default", "tournament_value", "scope", "owner", "baseline", "dependencies", "promotion_status"):
            if expected[name].get(field) != actual[name].get(field):
                errors.append(f"{name} registry field {field} is stale")
        for field in ("source_refs", "test_refs"):
            if actual[name].get("evidence", {}).get(field) != expected[name]["evidence"][field]:
                errors.append(f"{name} registry evidence {field} is stale")
            for evidence_path in actual[name].get("evidence", {}).get(field, []):
                if not (root / evidence_path).is_file():
                    errors.append(f"{name} evidence file missing: {evidence_path}")
    if current.get("build_inputs") != generated["build_inputs"]:
        errors.append("CMake build-input inventory or metadata is stale")
    if current.get("runtime_controls") != generated["runtime_controls"]:
        errors.append("runtime control inventory or metadata is stale")
    if current.get("configuration_rules") != generated["configuration_rules"]:
        errors.append("configuration rules are stale")

    option_names = set(expected)
    for rule in generated["configuration_rules"]:
        for name in [rule["when"], *rule["requires_all"], *rule["requires_any"], *rule["conflicts"]]:
            if name not in option_names:
                errors.append(f"configuration rule {rule['id']} references unknown option {name}")
        rule_engine = root / "cmake" / "ProtoddFeatureRules.cmake"
        if not rule_engine.is_file() or "configuration_rules" not in rule_engine.read_text(encoding="utf-8"):
            errors.append("configuration rules are not consumed by the CMake validator")
        for path in rule["evidence"]:
            if not (root / path).is_file():
                errors.append(f"configuration rule evidence file missing: {path}")
    control_source = "\n".join(
        p.read_text(encoding="utf-8", errors="ignore")
        for p in (root / "src" / "bwapi").glob("*.cpp")
    )
    found_controls = {
        name for name in re.findall(r'"bwapi-data/read/([^"/]+)"', control_source)
        if name.endswith("mode.txt") or name.endswith("opening.txt") or name.endswith("observe.txt")
    }
    registered_controls = {item["file"] for item in generated["runtime_controls"]}
    if found_controls != registered_controls:
        errors.append(f"runtime mode inventory differs: missing={sorted(found_controls-registered_controls)}, extra={sorted(registered_controls-found_controls)}")
    for control in generated["runtime_controls"]:
        expected_path = f'bwapi-data/read/{control["file"]}'
        if expected_path not in control_source:
            errors.append(f"runtime control is no longer read by the module: {expected_path}")
        for values in [control["tournament_values"], *control["purpose_values"].values()]:
            if not set(values).issubset(control["allowed_values"]):
                errors.append(f"{control['id']} contains a value outside its allowed set")
        for name in control["dependencies"]:
            if name not in option_names:
                errors.append(f"{control['id']} references unknown build option {name}")
        for names in control.get("dependencies_by_value", {}).values():
            for name in names:
                if name not in option_names:
                    errors.append(f"{control['id']} references unknown build option {name}")
        for path in control["evidence"]:
            if not (root / path).is_file():
                errors.append(f"{control['id']} evidence file missing: {path}")
    return errors


def registry_sha256(root: Path = ROOT) -> str:
    return hashlib.sha256((root / "docs" / "feature-registry.json").read_bytes()).hexdigest()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--write", action="store_true", help="regenerate the tracked registry")
    args = parser.parse_args()
    if args.write:
        REGISTRY_PATH.write_text(json.dumps(generate_registry(), indent=2) + "\n", encoding="utf-8")
    errors = validate_registry()
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1
    print(f"Feature registry valid: {len(generate_registry()['options'])} CMake options, "
          f"{len(RUNTIME_CONTROLS)} runtime controls, {len(CONFIGURATION_RULES)} config rules; "
          f"sha256={registry_sha256()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
