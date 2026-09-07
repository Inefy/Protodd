#!/usr/bin/env python3
"""Read-only live observer and portable decision reports. No third-party packages."""
from __future__ import annotations

import argparse
from collections import Counter, deque
from http.server import BaseHTTPRequestHandler, HTTPServer
import json
import math
import re
from pathlib import Path
import unittest


def number(value: str):
    try:
        result = float(value) if any(c in value for c in ".eE") else int(value)
        return result if math.isfinite(result) else None
    except ValueError:
        return value


def extras(fields):
    return {k: number(v) for field in fields if "=" in field for k, v in [field.split("=", 1)]}


SCOUT_REASONS = {"probe-harass-tag-worker": "Tag an exposed worker", "probe-harass-approach": "Approach an exposed worker", "probe-harass-evade-chaser": "Escape a chasing worker", "probe-harass-reset": "Open distance and prepare the next attack", "probe-harass-withdraw": "Return home: fighter observed or Probe endangered", "probe-scout-search": "Scout the enemy base"}


class DecisionTrace:
    def __init__(self):
        self.health = deque(maxlen=10000)
        self.events = deque(maxlen=5000)
        self.squads = deque(maxlen=12000)
        self.entities = deque(maxlen=720)
        self.strategies = deque(maxlen=5000)
        self.macro_history = deque(maxlen=20000)
        self.belief_history = deque(maxlen=720)
        self.losses = []
        self.phases = {}
        self.beliefs = {}
        self.macro = {}
        self.macro_since = {}
        self.macro_waits = Counter()
        self.strategy = {}
        self.malformed = 0
        self.frame = 0
        self.version = None
        self.map = "Waiting for game"
        self.retention_limited = False
        self.record_counts = Counter()
        self.worker_assignments = {}
        self.scouts = deque(maxlen=5000)
        self.decisions = {}
        self.transitions = deque(maxlen=12000)

    def append(self, rows, item):
        if len(rows) == rows.maxlen:
            self.retention_limited = True
        rows.append(item)

    def feed(self, line: str):
        fields = line.rstrip("\r\n").split(",")
        kind = fields[0]
        if kind == "START":
            self.__init__()
            self.map = re.sub(r"[\x00-\x1f]", "", fields[1]) if len(fields) > 1 else "Unknown map"
            return
        if kind == "DIAGNOSTICS":
            self.version = extras(fields[1:]).get("version")
            return
        supported = {"HEALTH", "SQUAD", "PHASE", "BELIEF", "ENTITY", "LOSS", "MACRO", "STRATEGY", "ORDER", "EVENT", "STATE", "WORKERS", "SCOUT"}
        if kind not in supported:
            return
        try:
            frame = int(fields[1])
            if frame < 0:
                raise ValueError("negative frame")
            self.frame = max(self.frame, frame)
            self.record_counts[kind] += 1
            if kind == "HEALTH":
                self.append(self.health, {"frame": frame, **extras(fields[2:])})
            elif kind == "WORKERS":
                self.worker_assignments = {"frame": frame, **extras(fields[2:])}
            elif kind == "SCOUT":
                row = {"frame": frame, "actor": int(fields[2]), "reason": fields[3],
                       "target": int(fields[4]), "accepted": bool(int(fields[5]))}
                self.append(self.scouts, row)
                self.append(self.events, {"frame": frame, "kind": kind, "actor": row["actor"],
                    "text": f'Probe #{row["actor"]}: {SCOUT_REASONS.get(row["reason"], row["reason"])} | target #{row["target"]} | ' +
                            ("accepted" if row["accepted"] else "not accepted")})
            elif kind == "SQUAD":
                row = {"frame": frame, "role": fields[2], **extras(fields[3:])}
                self.append(self.squads, row)
                key = row.get("key", row["role"])
                prior = self.decisions.get(key)
                if prior and prior.get("decision") != row.get("decision"):
                    transition = {"frame": frame, "key": key, "from": prior.get("decision"),
                        "to": row.get("decision"), "previous_duration_frames": frame - prior["since"],
                        "reason": row.get("reason"), "contact": row.get("enemies", 0) > 0}
                    self.append(self.transitions, transition)
                    self.append(self.events, {"frame": frame, "kind": "DECISION",
                        "text": f'{row["role"]} #{key}: {("engage", "kite", "retreat")[prior["decision"]] if prior.get("decision") in (0, 1, 2) else "unknown"} → {("engage", "kite", "retreat")[row["decision"]] if row.get("decision") in (0, 1, 2) else "unknown"} | {row.get("reason", "")}'})
                self.decisions[key] = {**row, "since": prior["since"] if prior and prior.get("decision") == row.get("decision") else frame}
                if len(self.decisions) > 256:
                    self.decisions = {k: v for k, v in self.decisions.items() if frame - v["frame"] <= 240}
            elif kind == "PHASE":
                calls, total, peak = map(int, fields[3:6])
                self.phases[fields[2]] = {"calls": calls, "mean_ms": total / max(1, calls) / 1000,
                                         "peak_ms": peak / 1000, "total_ms": total / 1000}
            elif kind == "BELIEF":
                probability = float(fields[3])
                if not math.isfinite(probability):
                    raise ValueError("invalid probability")
                self.beliefs[fields[2]] = probability
                if not self.belief_history or self.belief_history[-1]["frame"] != frame:
                    self.append(self.belief_history, {"frame": frame, "values": {}})
                self.belief_history[-1]["values"][fields[2]] = probability
            elif kind == "ENTITY":
                row = {"side": fields[2], "id": int(fields[3]), "kind": fields[4],
                       "x": int(fields[5]), "y": int(fields[6]), "hp": int(fields[7]),
                       "shields": int(fields[8]), "cooldown": int(fields[9]), "target": int(fields[10]),
                       "lastSeen": int(fields[11]), "visible": bool(int(fields[12])), "order": fields[13]}
                if not self.entities or self.entities[-1]["frame"] != frame:
                    self.append(self.entities, {"frame": frame, "units": []})
                self.entities[-1]["units"].append(row)
            elif kind == "LOSS":
                row = {"frame": frame, "side": fields[2], "id": int(fields[3]), "kind": fields[4],
                       "x": int(fields[5]), "y": int(fields[6]), "minerals": int(fields[7]), "gas": int(fields[8])}
                self.losses.append(row)
                self.append(self.events, {"frame": frame, "kind": kind,
                    "text": f'{row["side"]} lost {row["kind"]} #{row["id"]} at {row["x"]},{row["y"]}'})
            elif kind == "MACRO":
                key = "/".join(fields[2:5])
                row = {"frame": frame, "target": fields[3], "technology": int(fields[4]), "status": fields[5],
                       "accepted": bool(int(fields[6])), "reason": fields[7], "minerals": int(fields[8]),
                       "gas": int(fields[9]), "reserved": bool(int(fields[10])), "executable": bool(int(fields[11]))}
                previous = self.macro.get(key)
                if previous and not previous["accepted"]:
                    # A trace disappears when a goal completes/cancels. Never
                    # extrapolate an old wait across a missing heartbeat.
                    gap = frame - previous["frame"]
                    delta = max(0, gap) if gap <= 144 else 0
                    self.macro_waits[previous["target"] + ": " + previous["status"]] += delta
                if not previous or previous["status"] != row["status"] or frame - previous["frame"] > 144:
                    self.macro_since[key] = frame
                    self.append(self.events, {"frame": frame, "kind": kind,
                        "text": f'{row["target"]}: {row["status"]} — {row["reason"]}'})
                row["since"] = self.macro_since[key]
                row["key"] = key
                self.macro[key] = row
                self.append(self.macro_history, row)
            elif kind == "STRATEGY":
                row = {"frame": frame, "plan": fields[2], "proposed": fields[3], "posture": fields[4],
                       "enemy": fields[5], "mission": fields[6], "deferred": bool(int(fields[7]))}
                if any(self.strategy.get(k) != v for k, v in row.items() if k != "frame"):
                    self.append(self.events, {"frame": frame, "kind": kind,
                        "text": f'{row["plan"]} | {row["proposed"]} → {row["posture"]} | {row["mission"]}'})
                self.strategy = row
                self.append(self.strategies, row)
            elif kind == "ORDER":
                self.append(self.events, {"frame": frame, "kind": kind, "actor": int(fields[2]),
                    "text": f'#{fields[2]} {fields[7]} → #{fields[4]} / {fields[5]},{fields[6]} | ' +
                            ("accepted" if int(fields[8]) else "not accepted")})
            elif kind == "EVENT":
                self.append(self.events, {"frame": frame, "kind": fields[2], "text": ",".join(fields[3:])})
            elif kind == "STATE" and self.version is None:
                self.append(self.health, {"frame": frame, "minerals": int(fields[7]), "gas": int(fields[8]),
                    "supply": int(fields[9]), "supplyTotal": int(fields[10]), **extras(fields[14:])})
        except (ValueError, IndexError, TypeError):
            self.malformed += 1

    def summary(self):
        health = self.health[-1] if self.health else {}
        loss_value = {side: sum(x["minerals"] + x["gas"] for x in self.losses if x["side"] == side)
                      for side in ("self", "enemy")}
        # First accepted engagement per squad within a disjoint 10-second
        # window. Only callbacks for its actual members count as own losses.
        windows = []
        next_frame = {}
        attributed = set()
        for squad in self.squads:
            key = squad.get("key", squad["role"])
            if squad.get("enemies", 0) <= 0 or squad.get("decision") != 0 or squad.get("detectionBlocked"):
                continue
            if squad["frame"] < next_frame.get(key, -1):
                continue
            next_frame[key] = squad["frame"] + 240
            members = {int(x) for x in str(squad.get("members", "")).split(";") if x.isdigit()}
            losses = [x for x in self.losses if x["side"] == "self" and x["id"] in members and
                      squad["frame"] <= x["frame"] < squad["frame"] + 240 and
                      (x["id"], x["frame"]) not in attributed]
            if losses:
                attributed.update((x["id"], x["frame"]) for x in losses)
                windows.append({"frame": squad["frame"], "ratio": squad.get("ratio"),
                    "required": squad.get("required"), "reason": squad.get("reason"),
                    "own_losses_next_10s": len(losses), "own_resource_cost": sum(x["minerals"] + x["gas"] for x in losses),
                    "window_complete": self.frame >= squad["frame"] + 240})
        attempted = health.get("commandsAttempted")
        accepted = health.get("commandsAccepted")
        return {"diagnostics_version": self.version, "last_frame": self.frame,
            "supply_tight_seconds": health.get("supplyTightFrames", 0) / 24 if self.version else None,
            "idle_gateway_seconds": health.get("idleGatewayFrames", 0) / 24 if self.version else None,
            "idle_worker_seconds": health.get("idleWorkerFrames", 0) / 24 if self.version else None,
            "combat_command_acceptance": accepted / attempted if attempted else None,
            "combat_command_pipeline": {k: health.get(k) for k in ("commandsProposed", "commandsSuperseded",
                "commandsRedundant", "commandsDeferred", "commandsAttempted", "commandsAccepted")},
            "latest_worker_assignments": self.worker_assignments,
            "squad_decision_changes": len(self.transitions),
            "rapid_reentries": sum(t["to"] == 0 and t["from"] == 2 and t["contact"] and
                                   t["previous_duration_frames"] < 72 for t in self.transitions),
            "recent_decision_changes": list(self.transitions)[-12:],
            "observed_losses_resource_cost": loss_value if self.version else None,
            "macro_waits_seconds": {k: v / 24 for k, v in self.macro_waits.most_common(12)},
            "costly_engagement_windows": sorted(windows, key=lambda x: -x["own_resource_cost"])[:12],
            "phase_timings": self.phases, "malformed_records": self.malformed,
            "retention_limited": self.retention_limited, "record_counts": dict(self.record_counts)}

    def payload(self, manifest=None):
        return {"map": self.map, "frame": self.frame, "health": list(self.health),
            "events": list(self.events), "squads": list(self.squads), "entities": list(self.entities),
            "scouts": list(self.scouts),
            "strategies": list(self.strategies), "macroHistory": list(self.macro_history),
            "beliefHistory": list(self.belief_history),
            "beliefs": self.beliefs, "strategy": self.strategy,
            "macro": [v for v in self.macro.values() if self.frame - v["frame"] <= 144],
            "summary": self.summary(), "manifest": manifest,
            "outcome": verified_outcome(manifest)}


class LiveReader:
    def __init__(self, path: Path):
        self.path, self.offset, self.creation = path, 0, None
        self.pending = b""
        self.trace = DecisionTrace()

    def update(self):
        try:
            stat = self.path.stat()
            identity = (stat.st_dev, stat.st_ino)
            if stat.st_size < self.offset or self.creation != identity:
                self.trace = DecisionTrace()
                self.offset, self.pending = 0, b""
                self.creation = identity
            with self.path.open("rb") as handle:
                handle.seek(self.offset)
                data = handle.read()
                self.offset = handle.tell()
            lines = (self.pending + data).split(b"\n")
            self.pending = lines.pop()
            for line in lines:
                self.trace.feed(line.decode("utf-8", errors="replace"))
        except (FileNotFoundError, PermissionError):
            pass
        return self.trace


def read_manifest(path):
    try:
        result = json.loads(path.read_text(encoding="utf-8-sig"))
        return result if isinstance(result, dict) and "status" in result else None
    except (OSError, ValueError):
        return None


def verified_outcome(manifest):
    if not manifest or manifest.get("status") != "completed":
        return None
    found = re.fullmatch(r"END,(win|loss),(\d+)", str(manifest.get("result", "")))
    return found.group(1) if found else None


def analyze_decisions(lines):
    trace = DecisionTrace()
    for line in lines:
        trace.feed(line)
    return trace.summary()


def render(payload=None):
    template = Path(__file__).with_name("decision_viewer.html").read_text(encoding="utf-8")
    encoded = json.dumps(payload, ensure_ascii=True, allow_nan=False).replace("<", "\\u003c")
    return template.replace("__INITIAL_DATA__", encoded)


class DecisionReportTests(unittest.TestCase):
    def test_partial_and_replaced_live_file(self):
        import tempfile
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "game.log"
            reader = LiveReader(path)
            path.write_text("HEALTH,24,minerals=10")
            self.assertEqual(reader.update().frame, 0)
            with path.open("a") as handle:
                handle.write("\nHEALTH,48,minerals=20\n")
            self.assertEqual(reader.update().frame, 48)
            self.assertEqual(len(reader.update().health), 2)
            path.write_text("START,new\n")
            self.assertEqual(reader.update().map, "new")
            self.assertEqual(reader.trace.frame, 0)

    def test_member_losses_and_incomplete_window(self):
        trace = DecisionTrace()
        for line in ("DIAGNOSTICS,version=1", "SQUAD,100,main,key=7,enemies=2,decision=0,members=10;11;,ratio=1.8",
                     "LOSS,120,self,10,Dragoon,500,500,125,50", "LOSS,125,self,22,Probe,100,100,50,0"):
            trace.feed(line)
        window = trace.summary()["costly_engagement_windows"][0]
        self.assertEqual(window["own_losses_next_10s"], 1)
        self.assertFalse(window["window_complete"])

    def test_missing_telemetry_is_unknown(self):
        self.assertIsNone(DecisionTrace().summary()["idle_gateway_seconds"])
        trace = DecisionTrace()
        trace.feed("HEALTH,broken")
        trace.feed("BELIEF,10,Rush,nan")
        self.assertEqual(trace.malformed, 2)

    def test_macro_gap_not_counted_as_continuous_wait(self):
        trace = DecisionTrace()
        for frame in (24, 144, 2400):
            trace.feed(f"MACRO,{frame},0,Nexus,0,build-pending,0,expand,400,0,1,1")
        self.assertEqual(trace.summary()["macro_waits_seconds"]["Nexus: build-pending"], 5)

    def test_only_valid_runner_outcomes_count(self):
        self.assertIsNone(verified_outcome({"status": "completed", "result": None}))
        self.assertIsNone(verified_outcome({"status": "incomplete", "result": "END,loss,100"}))
        self.assertEqual(verified_outcome({"status": "completed", "result": "END,win,100"}), "win")

    def test_squad_changes_do_not_duplicate_casualties(self):
        trace = DecisionTrace()
        trace.feed("SQUAD,100,main,key=7,enemies=2,decision=0,members=10;11;")
        trace.feed("SQUAD,120,main,key=8,enemies=2,decision=0,members=10;11;")
        trace.feed("LOSS,140,self,10,Dragoon,500,500,125,50")
        self.assertEqual(len(trace.summary()["costly_engagement_windows"]), 1)

    def test_scrubbing_has_historical_strategy_and_beliefs(self):
        trace = DecisionTrace()
        trace.feed("STRATEGY,24,Opening,Hold,Hold,Unknown,Wait,0")
        trace.feed("STRATEGY,240,Push,Pressure,Pressure,FastTech,Expand,0")
        trace.feed("BELIEF,24,Unknown,0.9")
        trace.feed("BELIEF,240,Unknown,0.1")
        payload = trace.payload()
        self.assertEqual(payload["strategies"][0]["plan"], "Opening")
        self.assertEqual(payload["beliefHistory"][0]["values"]["Unknown"], 0.9)

    def test_embedded_log_cannot_inject_script(self):
        page = render({"map": "</script><script>alert(1)</script>"})
        self.assertNotIn("</script><script>alert", page)

    def test_scout_history_separates_proposal_and_acceptance(self):
        trace = DecisionTrace()
        trace.feed("SCOUT,100,7,probe-harass-tag-worker,9,1")
        trace.feed("SCOUT,120,7,probe-harass-evade-chaser,-1,0")
        payload = trace.payload()
        self.assertTrue(payload["scouts"][0]["accepted"])
        self.assertFalse(payload["scouts"][1]["accepted"])
        self.assertEqual(payload["events"][1]["kind"], "SCOUT")

    def test_decision_reentry_measures_elapsed_time_not_samples(self):
        trace = DecisionTrace()
        for frame in (100, 102, 124):
            trace.feed(f"SQUAD,{frame},main,key=7,enemies=2,decision=2,reason=regroup")
        trace.feed("SQUAD,148,main,key=7,enemies=2,decision=0,reason=attack")
        self.assertEqual(trace.summary()["rapid_reentries"], 1)
        self.assertEqual(trace.transitions[0]["previous_duration_frames"], 48)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="?")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--serve", type=int, metavar="PORT")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        unittest.main(argv=[__file__])
        return
    if args.log is None:
        parser.error("log is required")
    reader = LiveReader(args.log)
    manifest_path = args.manifest or args.log.with_suffix(".json")
    if args.serve is not None:
        class Handler(BaseHTTPRequestHandler):
            def do_GET(self):
                if self.path not in ("/", "/data"):
                    self.send_error(404)
                    return
                if self.path == "/data":
                    content = json.dumps(reader.update().payload(read_manifest(manifest_path)), allow_nan=False).encode()
                    mime = "application/json"
                else:
                    content, mime = render().encode(), "text/html; charset=utf-8"
                self.send_response(200)
                self.send_header("Content-Type", mime)
                self.send_header("Content-Length", str(len(content)))
                self.send_header("Cache-Control", "no-store")
                self.send_header("X-Content-Type-Options", "nosniff")
                self.end_headers()
                self.wfile.write(content)

            def log_message(self, *_):
                pass
        print(f"Decision observer: http://127.0.0.1:{args.serve}", flush=True)
        HTTPServer(("127.0.0.1", args.serve), Handler).serve_forever()
    else:
        trace = reader.update()
        output = args.output or args.log.with_suffix(".decisions.html")
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(render(trace.payload(read_manifest(manifest_path))), encoding="utf-8")
        print(json.dumps({"report": str(output.resolve()), **trace.summary()}, indent=2, allow_nan=False))


if __name__ == "__main__":
    main()
