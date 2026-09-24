import unittest

from training.whole_game_cadence_action_audit import score_window, summarize


class CadenceActionAuditTests(unittest.TestCase):
    def test_cadence_signature_checks_target_and_queued_argument(self):
        masks = dict(target_mode=True, target_entity=False, target_position=True,
                     unit_type=False, queued=True, order=False, technology=False,
                     upgrade=False, queue_slot=False)
        first = dict(frame=101, coordinate_space="pixel", loss_masks=masks,
                     actions=dict(kind="right_click", target_mode="position",
                                  target_position=[64, 96], queued=False),
                     actor_positive=[7])
        predicted = dict(target_mode="position", target_entity=None,
                         position_components=[(64.0, 96.0), (200.0, 200.0)],
                         position_component=0, queued=False, order=0,
                         unit_type=0, technology=0, upgrade=0, queue_slot=0)
        pending = dict(game_id="g", matchup="PvT", frame=96, event=1,
                       labels=[first], own_ids={7}, actor_id=7, visible_ids=set(),
                       predicted_kind="right_click", joint_kind="right_click",
                       joint_mode="position", event_probability=0.8,
                       argument_predictions=predicted)
        row = score_window(pending)
        self.assertEqual(row["target_position_error_px"], 0)
        self.assertTrue(row["full_signature_correct"])
        self.assertEqual(summarize([row], 1, 1)["full_signature_top1"], 1)
        far = score_window(dict(pending, argument_predictions=dict(
            predicted, position_component=1)))
        self.assertFalse(far["full_signature_correct"])
        self.assertGreater(far["target_position_error_px"], 64)

    def test_future_visible_target_is_not_scored_as_cadence_target(self):
        first = dict(frame=101, coordinate_space="pixel",
                     loss_masks=dict(target_mode=True, target_entity=True,
                                     target_position=False, unit_type=False, queued=False,
                                     order=False, technology=False, upgrade=False,
                                     queue_slot=False),
                     actions=dict(kind="right_click", target_mode="entity",
                                  target_entity=42), actor_positive=[7])
        pending = dict(game_id="g", matchup="PvT", frame=96, event=1,
                       labels=[first], own_ids={7}, actor_id=7, visible_ids=set(),
                       predicted_kind="right_click", joint_kind="right_click",
                       joint_mode="entity", event_probability=0.8,
                       argument_predictions=dict(target_mode="entity",
                           target_entity=None, position_components=[(0.0, 0.0)],
                           position_component=0, queued=False, order=0,
                           unit_type=0, technology=0, upgrade=0, queue_slot=0))
        scored = score_window(pending)
        self.assertFalse(scored["target_entity_known"])
        self.assertFalse(scored["full_signature_known"])

    def test_first_and_any_command_are_distinct(self):
        first = dict(frame=105, actions=dict(kind="train", target_mode="none"),
                     actor_positive=[7])
        second = dict(frame=110, actions=dict(kind="right_click", target_mode="position"),
                      actor_positive=[8])
        pending = dict(game_id="g", matchup="PvT", frame=96, event=1,
                       labels=[first, second], own_ids={7, 8}, actor_id=7,
                       predicted_kind="right_click", joint_kind="right_click",
                       joint_mode="position", event_probability=0.8)
        row = score_window(pending)
        self.assertEqual(row["first_action_lag"], 9)
        self.assertFalse(row["first_kind_correct"])
        self.assertTrue(row["any_kind_correct"])
        self.assertTrue(row["actor_correct"])
        self.assertFalse(row["joint_pair_correct"])
        self.assertEqual(summarize([row], 1, 1)["first_kind_top1"], 0)
        with self.assertRaisesRegex(ValueError, "disagree"):
            score_window(dict(pending, event=0))


if __name__ == "__main__":
    unittest.main()
