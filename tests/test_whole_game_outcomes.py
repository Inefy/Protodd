import unittest

from training.whole_game_outcomes import observed_progress, outcomes


def observation(frame, sequence, reason, *, position=(100, 100), queue=(),
                extra=(), researching=False, upgrading=False):
    actor = dict(id=1, relation=0, type=154, position=list(position),
                 own_state=dict(queue=list(queue)))
    return dict(frame=frame, sequence=sequence, reason=reason, perspective=0,
                entities=[actor, *extra], technology_completed=[0] * 44,
                technology_in_progress=[int(researching)] + [0] * 43,
                upgrade_levels=[0] * 61,
                upgrade_in_progress=[int(upgrading)] + [0] * 60)


def label(kind, *, frame=0, sequence=0, unit_type=None, position=None,
          technology=None, upgrade=None):
    return dict(frame=frame, observation_sequence=sequence, actor_positive=[1],
                coordinate_space="pixel", actions=dict(kind=kind, unit_type=unit_type,
                target_position=position, technology=technology, upgrade=upgrade))


class WholeGameOutcomeTests(unittest.TestCase):
    def test_build_train_research_upgrade_and_movement_have_distinct_positive_evidence(self):
        before = observation(0, 0, "before_command")
        building = dict(id=9, relation=0, type=156, position=[144, 136], own_state=dict(queue=[]))
        later = observation(24, 1, "cadence", position=(140, 100), queue=(64,),
                            extra=(building,), researching=True, upgrading=True)
        cases = [(label("build", unit_type=156, position=[128, 128]), "new_matching_structure_near_target"),
                 (label("train", unit_type=64), "actor_queue_gained_requested_unit"),
                 (label("research", technology=0), "requested_technology_progress"),
                 (label("upgrade", upgrade=0), "requested_upgrade_progress"),
                 (label("move", position=[200, 100]), "actor_moved_toward_target")]
        for command, expected in cases:
            self.assertEqual(observed_progress(command, before, later), expected)

    def test_missing_effect_is_unknown_and_next_actor_order_censors(self):
        before = observation(0, 0, "before_command")
        unchanged = observation(24, 1, "cadence")
        command = label("build", unit_type=156, position=[128, 128])
        self.assertIsNone(observed_progress(command, before, unchanged))
        timeline = [(before, dict(action=command, update_memory=False)),
                    (unchanged, dict(action=None, update_memory=True)),
                    (observation(30, 2, "before_command"),
                     dict(action=label("move", frame=30, sequence=2, position=[200, 100]),
                          update_memory=False)),
                    (observation(48, 3, "cadence"), dict(action=None, update_memory=True))]
        result = list(outcomes(timeline, 240))
        self.assertEqual(result[0]["status"], "superseded")
        self.assertEqual(result[0]["kind"], "build")
        self.assertEqual(result[1]["status"], "censored_tail")

    def test_horizon_without_progress_never_becomes_failed_label(self):
        before = observation(0, 0, "before_command")
        later = observation(24, 1, "cadence")
        rows = [(before, dict(action=label("train", unit_type=64), update_memory=False)),
                (later, dict(action=None, update_memory=True))]
        result = list(outcomes(rows, 24))
        self.assertEqual(result[0]["status"], "unobserved_within_window")


if __name__ == "__main__":
    unittest.main()
