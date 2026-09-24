import unittest

from training.whole_game_cadence_sequences import cadence_sequences, slot_targets


def row(frame, ids):
    return dict(frame=frame, entities=[dict(id=number, relation=0, visible=True)
                                      for number in ids])


def cadence(frame, ids, event):
    return row(frame, ids), dict(update_memory=True, event=event, action=None)


def command(frame, actor, kind="train", target=None):
    return row(frame, [actor]), dict(
        update_memory=False, event=None,
        action=dict(frame=frame, actor_positive=[actor],
                    actions=dict(kind=kind, target_entity=target),
                    loss_masks=dict(target_entity=target is not None)))


class CadenceSequencesTest(unittest.TestCase):
    def test_ordered_targets_are_separate_from_causal_context(self):
        windows = list(cadence_sequences([
            cadence(0, [1, 2], 1),
            command(5, 1), command(10, 2, "move", 99),
            cadence(24, [1, 2, 3], 1), command(30, 3),
            cadence(48, [1, 2, 3], None),
        ]))
        self.assertEqual(len(windows), 2)
        self.assertEqual(windows[0]["context"], [])
        self.assertEqual([x["actions"]["kind"] for x in windows[0]["labels"]],
                         ["train", "move"])
        self.assertEqual(windows[0]["actor_available"], [True, True])
        self.assertEqual(windows[0]["target_available"], [True, False])
        self.assertEqual(windows[1]["context"], [windows[0]["observation"]])
        self.assertEqual(windows[1]["actor_available"], [True])
        self.assertNotIn("labels", windows[1]["context"][0])
        targets = slot_targets(windows[0], maximum_commands=3)
        self.assertEqual([slot["delay_frames"] for slot in targets["slots"]],
                         [5, 10, None])
        self.assertEqual([slot["stop"] for slot in targets["slots"]],
                         [False, False, True])

    def test_future_actor_and_censored_window(self):
        windows = list(cadence_sequences([
            cadence(0, [1], 1), command(4, 2),
            cadence(24, [1, 2], None), command(27, 2),
        ]))
        self.assertEqual(len(windows), 1)
        self.assertEqual(windows[0]["actor_available"], [False])

    def test_event_mismatch_rejected(self):
        with self.assertRaises(ValueError):
            list(cadence_sequences([cadence(0, [1], 0), command(2, 1)]))

    def test_overflow_does_not_create_false_stop(self):
        sequence = list(cadence_sequences([
            cadence(0, [1], 1), command(2, 1), command(3, 1),
        ]))[0]
        targets = slot_targets(sequence, maximum_commands=1)
        self.assertEqual(targets["overflow_commands"], 1)
        self.assertEqual(len(targets["slots"]), 1)
        self.assertFalse(targets["slots"][0]["stop"])


if __name__ == "__main__":
    unittest.main()
