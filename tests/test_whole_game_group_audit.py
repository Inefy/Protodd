import unittest
import torch

from training.whole_game_group_audit import observed_fields


def fixture():
    label=dict(actor_positive=['a','b'],actor_negative=['c'],
               actions=dict(queued=True),loss_masks=dict(queued=True))
    sequence=dict(labels=[label],actor_available=[True])
    slot=dict(chosen_actor_set=torch.tensor([[True,True,False,False]]),
              queued=torch.tensor([[0.,2.]]))
    legacy=dict(predicted_commands=1,command_slots=[dict(full_signature_correct=True)])
    return sequence,[slot],dict(a=0,b=1,c=2,d=3),legacy


class ObservedFieldAuditTest(unittest.TestCase):
    def test_partial_actor_labels_preserve_unknown_members(self):
        args=fixture()
        args[1][0]['chosen_actor_set'][0,3]=True
        result=observed_fields(*args)
        self.assertEqual(result['covered_positive_memberships'],2)
        self.assertEqual(result['known_wrong_memberships'],0)
        self.assertEqual(result['unknown_memberships'],1)
        self.assertEqual(result['exact_confirmed_actor_sets'],0)
        self.assertEqual(result['all_observed_fields_correct'],0)

    def test_legacy_signature_does_not_excuse_wrong_queue_flag(self):
        args=fixture()
        args[1][0]['queued']=torch.tensor([[2.,0.]])
        result=observed_fields(*args)
        self.assertEqual(result['exact_confirmed_actor_sets'],1)
        self.assertEqual(result['queued_correct'],0)
        self.assertEqual(result['all_observed_fields_correct'],0)

    def test_stopped_slot_cannot_score_unissued_arguments_or_actors(self):
        args=fixture()
        args[3]['predicted_commands']=0
        result=observed_fields(*args)
        self.assertEqual(result['covered_positive_memberships'],0)
        self.assertEqual(result['queued_correct'],0)
        self.assertEqual(result['all_observed_fields_correct'],0)


if __name__=='__main__':
    unittest.main()
