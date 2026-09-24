import copy
import unittest

from training.whole_game_actor_dedupe import canonicalize_actor_selection
from training.whole_game_labels import label_command


class ActorDeduplicationTests(unittest.TestCase):
    def test_identical_replay_selection_duplicates_are_canonicalized(self):
        effect = dict(id=7, before_order=3, after_order=14, changed=True, removed=False)
        command = dict(schema="protodd-command-v1", frame=24, perspective=0,
                       observation_sequence=3, engine_returned=True,
                       selected_own=[7, 8, 7], acceptance="confirmed_transition",
                       semantic=dict(actor_effects=[effect, dict(effect)], decoded=True,
                                     target_requested=False, target_available=False,
                                     queued=False, target=-1, kind="attack_move",
                                     domain="unit_control", coordinate_space="pixel",
                                     position=[128, 128], order=14, unit_type=-1,
                                     technology=-1, upgrade=-1, queue_slot=-1))
        reference = dict(frame=24, perspective=0, sequence=3,
                         own={7: 3, 8: 3}, visible={7, 8})
        original = copy.deepcopy(command)
        with self.assertRaisesRegex(ValueError, "causal own-actor"):
            label_command(command, reference, 4096, 4096)
        canonical, changed = canonicalize_actor_selection(command)
        self.assertTrue(changed)
        self.assertEqual(command, original)
        self.assertEqual(canonical["selected_own"], [7, 8])
        self.assertEqual([e["id"] for e in canonical["semantic"]["actor_effects"]], [7])
        self.assertEqual(label_command(canonical, reference, 4096, 4096)["actor_positive"], [7])
        self.assertIs(canonicalize_actor_selection(canonical)[0], canonical)
        conflicting = copy.deepcopy(command)
        conflicting["semantic"]["actor_effects"][1]["changed"] = False
        with self.assertRaisesRegex(ValueError, "conflicting duplicate"):
            canonicalize_actor_selection(conflicting)


if __name__ == "__main__":
    unittest.main()
