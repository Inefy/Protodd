import copy
import unittest

from training.whole_game_labels import COMMAND_SCHEMA, label_command


class CommandLabels(unittest.TestCase):
    def fixture(self):
        reference = dict(frame=48, perspective=0, sequence=3, own={0:6, 1:6, 2:3}, visible={0,1,2,9})
        semantic = dict(kind="move", domain="unit_control", decoded=True, order=6, unit_type=-1,
                        technology=-1, upgrade=-1, queue_slot=-1, queued=False, target=-1,
                        target_requested=False, target_available=False, position=[120,160], coordinate_space="pixel",
                        actor_effects=[dict(id=0, before_order=6, after_order=6, changed=True, removed=False),
                                       dict(id=1, before_order=6, after_order=6, changed=False, removed=False)])
        command = dict(schema=COMMAND_SCHEMA, frame=48, perspective=0, observation_sequence=3,
                       engine_returned=True, selected_own=[0,1], acceptance="confirmed_transition", semantic=semantic)
        return command, reference

    def test_partial_selection_is_not_mislabeled_as_all_successful(self):
        command, reference = self.fixture()
        label = label_command(command, reference, 4096, 4096)
        self.assertEqual(label["actor_positive"], [0])
        self.assertEqual(label["actor_unknown"], [1])
        self.assertEqual(label["actor_negative"], [2])
        self.assertTrue(label["loss_masks"]["target_position"])
        self.assertFalse(label["loss_masks"]["technology"])
        self.assertIsNone(label["actions"]["technology"])
        self.assertFalse(label["command_completion_verified"])

    def test_unknown_unit_target_is_masked_not_a_negative_or_location_label(self):
        command, reference = self.fixture()
        command["semantic"]["target_requested"] = True
        label = label_command(command, reference, 4096, 4096)
        for field in ("target_mode", "target_entity", "target_position"):
            self.assertFalse(label["loss_masks"][field])
            self.assertIsNone(label["actions"][field])
        self.assertTrue(label["loss_masks"]["kind"])

    def test_actor_transition_outweighs_unreliable_aggregate_return(self):
        command, reference = self.fixture()
        command["engine_returned"] = False
        command["semantic"].update(kind="unload_all", domain="transport", position=None)
        self.assertEqual(label_command(command, reference,4096,4096)["actor_positive"],[0])
        for effect in command["semantic"]["actor_effects"]:
            effect["changed"] = False
        command["acceptance"] = "engine_rejected"
        self.assertIsNone(label_command(command, reference,4096,4096))
        command["engine_returned"] = True
        command["acceptance"] = "unconfirmed_no_change"
        self.assertIsNone(label_command(command, reference,4096,4096))

    def test_fabricated_effects_hidden_targets_and_future_links_rejected(self):
        command, reference = self.fixture()
        cases = []
        row = copy.deepcopy(command); row["frame"] += 1; cases.append(row)
        row = copy.deepcopy(command); row["semantic"]["actor_effects"][0]["before_order"] = 7; cases.append(row)
        row = copy.deepcopy(command); row["semantic"].update(target=90,target_requested=True,target_available=True); cases.append(row)
        row = copy.deepcopy(command); row["semantic"].update(target=9,target_requested=True,target_available=False); cases.append(row)
        row = copy.deepcopy(command); row["acceptance"] = "engine_rejected"; cases.append(row)
        row = copy.deepcopy(command); row["semantic"]["position"] = [-1,20]; cases.append(row)
        row = copy.deepcopy(command); row["semantic"]["actor_effects"][0]["id"] = 9; cases.append(row)
        for row in cases:
            with self.assertRaises(ValueError):
                label_command(row,reference,4096,4096)


if __name__ == '__main__':
    unittest.main()
