import unittest

import torch

from training.whole_game_model import KINDS
from training.whole_game_multislot_audit import (
    early_mass_probe_moves, score_sequence, summarize)


class MultiSlotAuditTest(unittest.TestCase):
    def test_repeated_probe_moves_are_detected_before_live_games(self):
        moves = [dict(actor=actor, x=1536, y=2048)
                 for actor in (4, 6, 10, 15)]
        rows = [dict(game_id="same", frame=frame, early_probe_moves=moves)
                for frame in (7, 31, 55)]
        self.assertEqual(early_mass_probe_moves(rows), 1)
        rows[2]["game_id"] = "other"
        self.assertEqual(early_mass_probe_moves(rows), 0)

    def test_free_running_slot_records_probe_move_target(self):
        outputs = [dict(chosen_stop=torch.tensor([0]),
                        chosen_actor=torch.tensor([actor]),
                        chosen_kind=torch.tensor([KINDS.index(
                            "move" if actor % 2 == 0 else "right_click")]),
                        chosen_target_mode=torch.tensor([2]),
                        chosen_position=torch.tensor([[0.5, 0.5]]))
                   for actor in range(4)]
        sequence = dict(observation=dict(
            frame=7, entities=[dict(id=actor + 1, type=64, relation=0)
                               for actor in range(4)]), labels=[])
        row = score_sequence(sequence, outputs,
                             {actor + 1: actor for actor in range(4)}, 96, 128)
        self.assertEqual(row["early_probe_moves"], [
            dict(actor=actor + 1, x=1536, y=2048) for actor in range(4)])

    def test_exact_first_command_and_stop(self):
        label = dict(frame=29, actor_positive=[1], coordinate_space="pixel",
                     actions=dict(kind="train", target_mode="none",
                                  target_entity=None, target_position=None,
                                  unit_type=64),
                     loss_masks=dict(target_mode=True, target_entity=False,
                                     target_position=False, unit_type=True))
        sequence = dict(observation=dict(frame=24), labels=[label],
                        actor_available=[True], target_available=[True])
        action = dict(chosen_stop=torch.tensor([0]),
                      chosen_actor=torch.tensor([0]),
                      chosen_kind=torch.tensor([KINDS.index("train")]),
                      chosen_target_mode=torch.tensor([0]),
                      chosen_target_entity=torch.tensor([-1]),
                      chosen_position=torch.zeros((1, 2)),
                      chosen_delay=torch.tensor([5]),
                      chosen_unit_type=torch.tensor([64]))
        stop = dict(chosen_stop=torch.tensor([1]))
        row = score_sequence(sequence, [action, stop], {1: 0}, 128, 128)
        self.assertEqual(row["predicted_commands"], 1)
        self.assertTrue(row["command_slots"][0]["full_signature_correct"])
        row["predicted_kind_indices"] = [KINDS.index("train")]
        summary = summarize([row])
        self.assertEqual(summary["first_kind_correct"], 1)
        self.assertEqual(summary["full_signature_correct"], 1)

    def test_early_stop_marks_command_missed(self):
        sequence = dict(observation=dict(frame=0), labels=[
            dict(frame=4, actor_positive=[1], coordinate_space="pixel",
                 actions=dict(kind="move", target_mode="position",
                              target_entity=None, target_position=[64, 64],
                              unit_type=None),
                 loss_masks=dict(target_mode=True, target_entity=False,
                                 target_position=True, unit_type=False))],
                        actor_available=[True], target_available=[True])
        row = score_sequence(sequence, [dict(chosen_stop=torch.tensor([1]))],
                             {1: 0}, 128, 128)
        self.assertEqual(row["predicted_commands"], 0)
        self.assertFalse(row["command_slots"][0]["kind_correct"])
        self.assertIsNone(row["command_slots"][0]["target_position_error_px"])


if __name__ == "__main__":
    unittest.main()
