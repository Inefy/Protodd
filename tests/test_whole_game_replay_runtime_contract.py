import re
import unittest
from pathlib import Path

from training.whole_game_labels import COMMAND_SCHEMA, label_command

ROOT = Path(__file__).resolve().parents[1]


def label_fixture(kind, domain):
    reference = dict(frame=48, perspective=0, sequence=3, own={10: 6}, visible={10})
    semantic = dict(
        kind=kind, domain=domain, decoded=True, order=-1, unit_type=-1,
        technology=-1, upgrade=-1, queue_slot=-1, queued=False,
        target=-1, target_requested=False, target_available=False,
        position=None, coordinate_space="pixel",
        actor_effects=[dict(id=10, before_order=6, after_order=7, changed=True, removed=False)],
    )
    command = dict(
        schema=COMMAND_SCHEMA, frame=48, perspective=0, observation_sequence=3,
        engine_returned=True, selected_own=[10], acceptance="confirmed_transition",
        semantic=semantic,
    )
    return label_command(command, reference, 4096, 4096)


class ReplayRuntimeCommandContracts(unittest.TestCase):
    def test_replay_train_fighter_packet_carries_no_unit_type(self):
        source = (ROOT / "tools/replay_native/whole_game_actions.hpp").read_text()
        packet39 = re.search(r"case 39:(.*?)(?=case 40:)", source, re.S)
        self.assertIsNotNone(packet39)
        self.assertIn('kind("train_fighter", "production")', packet39.group(1))
        self.assertNotIn("unitType", packet39.group(1))

    def test_train_fighter_label_does_not_supervise_unit_type(self):
        label = label_fixture("train_fighter", "production")
        self.assertIsNone(label["actions"]["unit_type"])
        self.assertFalse(label["loss_masks"]["unit_type"])
        self.assertEqual(label["actions"]["target_mode"], "none")
        self.assertTrue(label["loss_masks"]["target_mode"])

    def test_replay_archon_merge_packet_carries_no_partner_target(self):
        source = (ROOT / "tools/replay_native/whole_game_actions.hpp").read_text()
        packet42 = re.search(r"case 42:(.*?)(?=case 90:)", source, re.S)
        self.assertIsNotNone(packet42)
        self.assertIn('kind("merge_archon", "ability")', packet42.group(1))
        self.assertNotIn("resolveTarget", packet42.group(1))

    def test_archon_merge_label_has_none_target_mode_and_no_target_entity(self):
        label = label_fixture("merge_archon", "ability")
        self.assertEqual(label["actions"]["target_mode"], "none")
        self.assertTrue(label["loss_masks"]["target_mode"])
        self.assertIsNone(label["actions"]["target_entity"])
        self.assertFalse(label["loss_masks"]["target_entity"])

if __name__ == "__main__":
    unittest.main()
