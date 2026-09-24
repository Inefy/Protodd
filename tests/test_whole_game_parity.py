import copy
import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_parity import compare
from training.whole_game_pilot import SCHEMA


class WholeGameParityTests(unittest.TestCase):
    def test_observed_comparison_and_private_state_rejection(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            unit = dict(id=0, type=7, relation=0, position=[40, 50], hp=20, shields=10,
                        visible=True, completed=True, first_seen=0, last_seen=24,
                        own_state=dict(energy=0, ground_cooldown=0, air_cooldown=0,
                                       order=6, order_target=-1, queue=[], cargo=[]))
            base = dict(schema=SCHEMA, reason="cadence", frame=24, minerals=50, gas=0,
                        supply_used=8, supply_total=18, technology_completed=[0],
                        technology_in_progress=[0], upgrade_levels=[0], upgrade_in_progress=[0],
                        vision="20", entities=[unit])
            next_row = copy.deepcopy(base)
            next_row["frame"] = 48
            next_row["entities"][0]["last_seen"] = 48
            def write(path, rows):
                path.write_text("".join(json.dumps(r) + "\n" for r in rows))
            live, replay = root / "live.jsonl", root / "replay.jsonl"
            write(live, [base, next_row]); write(replay, [base, next_row])
            terrain = root / "live-terrain.json"
            other_terrain = root / "replay-terrain.json"
            t = dict(schema="protodd-terrain-v2", width_walktiles=2,
                     height_walktiles=2, walkability="1100")
            terrain.write_text(json.dumps(t)); other_terrain.write_text(json.dumps(t))
            result = compare(live, replay, terrain, other_terrain)
            self.assertEqual(result["aligned_frames"], 2)
            self.assertEqual(result["matching_entities"], 2)
            self.assertEqual(result["walkability_matches"], 4)
            hidden = copy.deepcopy(base)
            hidden["entities"][0]["relation"] = 1
            write(live, [hidden, next_row])
            with self.assertRaisesRegex(ValueError, "private state"):
                compare(live, replay, terrain, other_terrain)


if __name__ == "__main__":
    unittest.main()
