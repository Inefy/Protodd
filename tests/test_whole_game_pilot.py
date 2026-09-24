import copy
import json
from pathlib import Path
import tempfile
import unittest

from training.whole_game_pilot import SCHEMA, analyse, select_games
from training.whole_game_labels import COMMAND_SCHEMA, materialize


class WholeGamePilotTests(unittest.TestCase):
    def test_selection_preserves_split_and_exact_player_quality(self):
        games = []
        for enemy in ("T", "Z", "P"):
            for split, quality in (("test", "qualified_ladder"), ("validation", "qualified_ladder"),
                                   ("train", "unknown"), ("train", "qualified_ladder")):
                games.append(dict(split=split, races=["P", enemy], player_quality=[quality, "unknown"]))
        selected = select_games(dict(games=games))
        self.assertEqual({g["matchup"] for g in selected}, {"PvT", "PvZ", "PvP"})
        self.assertTrue(all(g["game"]["split"] == "train" and
                            g["game"]["player_quality"][g["perspective"]] == "qualified_ladder"
                            for g in selected))
        with self.assertRaises(ValueError):
            select_games(dict(games=games[:-1]))

    def fixture(self, root):
        summary = dict(schema=SCHEMA, complete=True, width_tiles=2, height_tiles=1,
                       valid_through_frame=24, observations=1, commands=1,
                       command_schema=COMMAND_SCHEMA, technology_count=1, upgrade_count=1)
        entity = dict(id=0, relation=0, first_seen=0, last_seen=24, visible=1,
                      own_state=dict(order_target=-1, cargo=[], order=6))
        observation = dict(schema=SCHEMA, perspective=0, sequence=0, frame=24,
                           vision="21", entities=[entity], reason="before_command",
                           technology_completed=[0], technology_in_progress=[0],
                           upgrade_levels=[0], upgrade_in_progress=[0])
        command = dict(perspective=0, frame=24, observation_sequence=0, selected_own=[0],
                       acceptance="confirmed_transition", engine_returned=True, payload_hex="00000100", code=97,
                       schema=COMMAND_SCHEMA, semantic=dict(kind="move", domain="unit_control", decoded=True,
                           order=6, unit_type=-1, technology=-1, upgrade=-1, queue_slot=-1, queued=False,
                           target=-1, target_requested=False, target_available=False, position=[10,10],
                           coordinate_space="pixel", actor_effects=[dict(id=0, before_order=6, after_order=6, changed=True, removed=False)]))
        (root / "summary.json").write_text(json.dumps(summary))
        (root / "terrain.json").write_text(json.dumps(dict(schema="protodd-terrain-v2",
            width_walktiles=8, height_walktiles=4, walkability="1"*32, height="0"*32)))
        return observation, command

    def test_causal_references_and_privacy_are_required_even_in_pilots(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            observation, command = self.fixture(root)
            def write(o, c):
                (root / "observations.jsonl").write_text(json.dumps(o) + "\n")
                (root / "commands.jsonl").write_text(json.dumps(c) + "\n")
            write(observation, command)
            self.assertFalse(analyse(root)["training_ready"])
            mutations = [
                ("own_state", dict(order_target=9000, cargo=[], order=6)),
                ("relation", 1), ("last_seen", 25),
            ]
            for key, value in mutations:
                changed = copy.deepcopy(observation)
                changed["entities"][0][key] = value
                write(changed, command)
                with self.assertRaises(ValueError):
                    analyse(root)
            for key, value in (("selected_own", [9]), ("frame", 23),
                               ("observation_sequence", 1), ("acceptance", "accepted")):
                write(observation, dict(command, **{key: value}))
                with self.assertRaises(ValueError):
                    analyse(root)

    def test_identical_duplicate_actor_evidence_is_canonicalized(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            observation, command = self.fixture(root)
            command["selected_own"].append(0)
            command["semantic"]["actor_effects"].append(
                copy.deepcopy(command["semantic"]["actor_effects"][0]))
            (root / "observations.jsonl").write_text(json.dumps(observation) + "\n")
            (root / "commands.jsonl").write_text(json.dumps(command) + "\n")
            self.assertEqual(analyse(root)["candidate_kinds"]["move"], 1)
            labels = root / "labels.jsonl"
            self.assertEqual(materialize(root, labels)["candidate_labels"], 1)
            self.assertEqual(json.loads(labels.read_text())["actor_positive"], [0])
            command["semantic"]["actor_effects"][-1]["changed"] = False
            (root / "commands.jsonl").write_text(json.dumps(command) + "\n")
            with self.assertRaisesRegex(ValueError, "conflicting duplicate"):
                analyse(root)


if __name__ == "__main__":
    unittest.main()
