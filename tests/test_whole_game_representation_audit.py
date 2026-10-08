import unittest

from training.whole_game_representation_audit import (
    _Summary, _validate_split_identity)


def label(frame, kind, actors, *, target_entity=-1, target_position=None,
          coordinate_space="pixel", target_mode="none", target_entity_known=False):
    return {
        "frame": frame,
        "actor_positive": actors,
        "coordinate_space": coordinate_space,
        "actions": {"kind": kind, "target_mode": target_mode,
                    "target_entity": target_entity, "target_position": target_position},
        "loss_masks": {"target_mode": True, "target_entity": target_entity_known,
                       "target_position": target_position is not None},
    }


class WholeGameRepresentationAuditTests(unittest.TestCase):
    def test_split_guard_rejects_game_and_replay_overlap(self):
        train = {"selected": [{"split": "train", "game_id": "game-a",
                               "replay_sha256": "replay-a"}]}
        validation = {"selected": [{"split": "validation", "game_id": "game-b",
                                    "replay_sha256": "replay-b"}]}
        _validate_split_identity(train, validation)
        with self.assertRaisesRegex(ValueError, "identities overlap"):
            _validate_split_identity(
                train, {"selected": [{"split": "validation", "game_id": "game-a",
                                       "replay_sha256": "replay-b"}]})
        with self.assertRaisesRegex(ValueError, "replay assets overlap"):
            _validate_split_identity(
                train, {"selected": [{"split": "validation", "game_id": "game-b",
                                       "replay_sha256": "replay-a"}]})

    def test_event_conditioned_slots_availability_delay_and_position(self):
        action = label(97, "attack_move", [1, 2], target_position=[20, 30])
        target = label(105, "right_click", [2], target_entity=3,
                       target_mode="entity", target_entity_known=True)
        action_sequence = {
            "event": True,
            "observation": {"frame": 96, "entities": [
                {"id": 1, "relation": 0, "visible": True},
                {"id": 2, "relation": 0, "visible": True},
                {"id": 3, "relation": 1, "visible": True},
            ]},
            "labels": [action, target],
            "actor_available": [True, True],
            "target_available": [True, True],
        }
        no_action_sequence = {"event": False, "observation": {"frame": 120, "entities": []},
                              "labels": [], "actor_available": [], "target_available": []}

        summary = _Summary()
        summary.observe_cadence(False, within_frame_limit=True)
        summary.observe_cadence(False, within_frame_limit=True)
        summary.observe_cadence(True, within_frame_limit=True)
        summary.observe_cadence(True, within_frame_limit=False)
        summary.add_sequence(action_sequence, maximum_slots=6, window=24,
                             map_width_px=640, map_height_px=480)
        summary.add_sequence(no_action_sequence, maximum_slots=6, window=24,
                             map_width_px=640, map_height_px=480)
        report = summary.report()

        self.assertEqual(report["complete_windows_within_frame_limit"], 2)
        self.assertEqual(report["censored_windows_within_frame_limit"], 1)
        self.assertEqual(report["censored_final_windows_full_game"], 2)
        self.assertEqual(report["event_conditioned_windows"]["action"], 1)
        self.assertEqual(report["event_conditioned_windows"]["no_action"], 1)
        action_metrics = report["by_event"]["action"]
        self.assertEqual(action_metrics["active_slots_by_ordinal"], {"0": 1, "1": 1})
        self.assertEqual(action_metrics["stop_slots_by_ordinal"], {"2": 1})
        self.assertEqual(action_metrics["actor_group_size"], {"1": 1, "2": 1})
        self.assertEqual(action_metrics["entity_target_availability"], {"available": 1})
        self.assertEqual(action_metrics["delay_frames"]["bins"], {"0-1": 1, "6-11": 1})
        self.assertEqual(action_metrics["position_labels"]["in_map_bounds"], 1)
        no_action_metrics = report["by_event"]["no_action"]
        self.assertEqual(no_action_metrics["active_slots_by_ordinal"], {})
        self.assertEqual(no_action_metrics["stop_slots_by_ordinal"], {"0": 1})

    def test_rejects_actor_or_target_availability_that_is_not_causal(self):
        hidden_target = label(12, "right_click", [1], target_entity=9,
                              target_mode="entity", target_entity_known=True)
        sequence = {"event": True,
                    "observation": {"frame": 12, "entities": [
                        {"id": 1, "relation": 0, "visible": True},
                        {"id": 9, "relation": 1, "visible": False},
                    ]},
                    "labels": [hidden_target], "actor_available": [True],
                    "target_available": [True]}
        with self.assertRaisesRegex(ValueError, "target availability"):
            _Summary().add_sequence(sequence, maximum_slots=6, window=24,
                                    map_width_px=640, map_height_px=480)


if __name__ == "__main__":
    unittest.main()
