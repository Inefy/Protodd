"""Checks that the control audit counts sampled frames and finished games."""

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.control_campaign_review import audit_log, campaign  # noqa: E402


TRACE = """\
STRATEGY,120,Opening,Hold,Hold,Unknown,No mission,0,strategyPosture=Pressure,policyEnabled=1,policyAction=3,policyWeights=0,coveredPressureRelease=0
SQUAD,120,MainArmy,key=1,units=35,enemies=4,ratio=2.5,travelReason=assemble-at-rally,detectionBlocked=1
SQUAD,120,MainArmy,key=2,units=31,travelReason=assemble-at-rally,detectionBlocked=1
SQUAD,120,MainArmy,key=3,units=20,travelReason=cover-expansion,detectionBlocked=0
SQUAD,120,BaseDefense,key=4,units=5,enemies=6,travelReason=squad-mission
ORDER,120,7,0,-1,10,20,detector-escort,1
ORDER,120,8,0,-1,30,40,detector-escort,1
ORDER,120,9,1,741,-1,-1,detector-wait-volley,1
ORDER,240,9,0,-1,10,20,wait-for-mobile-detection,1
DETECTOR_ALLOC,120,blockedMain=2,observers=2,escorts=2
EVENT,120,forward-third-screen,site=400x1000
EVENT,120,base-threat-expansion-guard
EVENT,120,emergency-defense-consolidation
EVENT,120,safe-remote-mining
EVENT,120,pvt-contain-break,target=300x1700
STATE,120,Opening,Hold,Unknown,army=37,nexuses=3,probes=42,selfComp=Nexus=3/2
STRATEGY,240,Opening,Pressure,Pressure,Unknown,No mission,0,strategyPosture=Pressure,policyEnabled=1,policyAction=3,policyWeights=0,coveredPressureRelease=1,containBreak=1
SQUAD,240,MainArmy,key=1,units=35,travelReason=attack-target,detectionBlocked=0
SQUAD,240,MainArmy,key=3,units=20,travelReason=cover-expansion,detectionBlocked=0
STATE,240,Opening,Pressure,Unknown,army=42,nexuses=2,probes=35,selfComp=Nexus=2/1
"""


class ControlCampaignReviewTests(unittest.TestCase):
    def test_sampled_frames_not_squad_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "Protodd.log"
            log.write_text(TRACE)
            self.assertEqual(audit_log(log), {
                "empty_policy_pressure_hold_ticks": 1,
                "covered_pressure_release_ticks": 1,
                "forward_third_screen_ticks": 1,
                "forward_third_event_ticks": 1,
                "base_threat_expansion_guard_ticks": 1,
                "emergency_defense_consolidation_ticks": 1,
                "safe_remote_mining_event_ticks": 1,
                "pvt_contain_break_event_ticks": 1,
                "contain_break_strategy_ticks": 1,
                "large_base_threat_ticks": 1,
                "large_base_threat_mean_defense_share": 0.055,
                "large_base_threat_with_main_ticks": 1,
                "large_base_threat_with_main_mean_defense_share": 0.055,
                "large_base_threat_with_force_ticks": 1,
                "large_base_threat_with_force_mean_defense_share": 0.055,
                "contested_reserve_release_ticks": 1,
                "main_detection_blocked_ticks": 1,
                "multiple_main_groups_blocked_ticks": 1,
                "dual_escort_order_frames": 1,
                "detector_wait_volley_frames": 1,
                "detector_wait_retreat_frames": 1,
                "dual_escort_with_multiple_blocked_main": 1,
                "large_main_rally_ticks": 1,
                "favorable_main_rally_ticks": 1,
                "favorable_main_detection_blocked_ticks": 1,
                "large_main_route_ticks": {
                    "assemble-at-rally": 1, "attack-target": 1},
                "peak_army": 42,
                "peak_probes": 42,
                "first_three_nexus_frame": 120,
                "nexus_count_drop_frames_with_army": [240],
            })

    def test_campaign_needs_normal_results_and_received_logs(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / "server"
            root.mkdir()
            (root / "games.jsonl").write_text("{}\n{}\n")
            rows = [dict(gameID=0, reportingBot="Protodd", gameEndType="NORMAL",
                         won=False, finalFrame=300, map="Benzene"),
                    dict(gameID=1, reportingBot="Protodd", gameEndType="NORMAL",
                         won=True, finalFrame=400, map="Destination")]
            (root / "results.jsonl").write_text("\n".join(map(json.dumps, rows)))
            log = (root / "replays" / "bot-write" / "game-0" /
                   "Protodd" / "received" / "Protodd.log")
            log.parent.mkdir(parents=True)
            log.write_text(TRACE)
            self.assertFalse(campaign(Path(directory))["complete"])
            second = (root / "replays" / "bot-write" / "game-1" /
                      "Protodd" / "received" / "Protodd.log")
            second.parent.mkdir(parents=True)
            second.write_text(TRACE)
            self.assertTrue(campaign(Path(directory))["complete"])


if __name__ == "__main__":
    unittest.main()
