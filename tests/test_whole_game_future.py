import unittest

from training.whole_game_future import derive, future_targets
from training.whole_game_model import DOMAINS


def observation(frame, vision, *, minerals=50, enemy=False):
    entities = [dict(id=0, relation=0, hp=40, shields=20, visible=1)]
    if enemy:
        entities.append(dict(id=1, relation=1, hp=30, shields=0, visible=1))
    return dict(frame=frame, reason="cadence", perspective=0, vision=vision,
                entities=entities, minerals=minerals, gas=0, supply_used=10,
                technology_completed=[0, 0], upgrade_levels=[0])


class WholeGameFutureTests(unittest.TestCase):
    def test_future_targets_use_only_later_legal_observation_and_censor_tail(self):
        first = observation(0, "0012")
        later = observation(24, "1212", minerals=70, enemy=True)
        final = observation(48, "1212", minerals=90, enemy=True)
        command = dict(frame=23, reason="before_command", perspective=0)
        timeline = [(first, dict(update_memory=True, action=None)),
                    (command, dict(update_memory=False, action=dict(domain="economy"))),
                    (later, dict(update_memory=True, action=None)),
                    (final, dict(update_memory=True, action=None))]
        result = list(future_targets(timeline, (24, 48)))
        self.assertEqual([(source["frame"], target["target_frame"]) for source, target in result],
                         [(0, 24), (24, 48), (0, 48)])
        first_target = result[0][1]
        self.assertEqual(first_target["newly_explored_tiles"], 2)
        self.assertEqual(first_target["newly_known_enemy_ids_retained"], 1)
        self.assertEqual(first_target["minerals_delta"], 20)
        self.assertEqual(first_target["confirmed_command_domains"][DOMAINS.index("economy")], 1)
        self.assertEqual(sum(result[1][1]["confirmed_command_domains"]), 0)
        self.assertEqual(result[2][1]["confirmed_command_domains"][DOMAINS.index("economy")], 1)

    def test_rejects_noncausal_or_regressed_vision(self):
        first = observation(0, "1212")
        later = observation(24, "0012")
        with self.assertRaisesRegex(ValueError, "regressed"):
            derive(first, later, ())
        with self.assertRaisesRegex(ValueError, "same perspective"):
            derive(later, first, ())


if __name__ == "__main__":
    unittest.main()
