"""Checks loss windows and accepted-command attribution in the control audit."""

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.control_outcome_audit import audit_log  # noqa: E402


def state(frame: int, probes: int, army: int) -> str:
    return (f"STATE,{frame},plan,Hold,Unknown,0,0,0,0,0,0,0,normal,idle,"
            f"pylons=1/1,probes={probes},army={army},nexuses=2,"
            "attackTarget=3000x3000\n")


class ControlOutcomeAuditTests(unittest.TestCase):
    def test_incident_links_expansion_route_and_accepted_orders(self):
        trace = (state(1000, 40, 30) +
                 "SQUAD,1000,MainArmy,units=26,enemies=4,ratio=2,decision=0,"
                 "travelReason=cover-expansion,detectionBlocked=0,center=1000x1000\n"
                 "ORDER,1100,7,0,-1,1200,1000,defense-return,1\n"
                 "ORDER,1100,8,0,-1,1200,1000,combat-retreat,0\n" +
                 "ACTION,1101,19,worker-evacuate,Move,issued,accepted,target=-1\n" +
                 state(1720, 30, 29))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Protodd.log"
            path.write_text(trace)
            report = audit_log(path)
        incident = next(row for row in report["incidents"] if row["kind"] == "probes")
        self.assertEqual(incident["lost"], 10)
        self.assertEqual(incident["main_routes"], {"cover-expansion": 1})
        self.assertEqual(incident["accepted_orders"], {"defense-return": 1})
        self.assertEqual(incident["workers_evacuated"], 1)

    def test_uncontested_army_without_progress_is_reported(self):
        trace = ("SQUAD,1000,MainArmy,units=20,enemies=0,ratio=3,decision=0,"
                 "travelReason=attack-target,detectionBlocked=0,center=100x100,"
                 "travelGoal=2000x100\n"
                 "SQUAD,1720,MainArmy,units=20,enemies=0,ratio=3,decision=0,"
                 "travelReason=attack-target,detectionBlocked=0,center=150x100,"
                 "travelGoal=2000x100\n")
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "Protodd.log"
            path.write_text(trace)
            report = audit_log(path)
        self.assertEqual(len(report["assault_stalls"]), 1)
        self.assertEqual(report["assault_stalls"][0]["units"], 20)


if __name__ == "__main__":
    unittest.main()
