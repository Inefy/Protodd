import unittest

from training.whole_game_group_gates import development_gates


def evidence():
    reference = dict(free_running=dict(slot_commands=100, full_signature_correct=2,
        position_known=50, position_within_64px=2, position_median_error_px=500,
        non_right_click_correct=5), by_matchup={m: dict(full_signature_correct=1)
            for m in ('PvP','PvT','PvZ')})
    candidate = dict(free_running=dict(slot_commands=100, full_signature_correct=10,
        position_known=50, position_within_64px=15, position_median_error_px=100,
        non_right_click_correct=10), by_matchup={m: dict(full_signature_correct=3)
            for m in ('PvP','PvT','PvZ')}, decoded_actor_sets=dict(known_actor_true_positives=60,
        known_actor_false_positives=10, known_actor_false_negatives=40),
        observed_field_audit_v2=dict(commands=100,all_observed_fields_correct=5),
        combat_actor_kind={k:dict(samples=10,actor_kind_correct=2) for k in ('attack','attack_move')})
    return reference, candidate


class DevelopmentGateTest(unittest.TestCase):
    def test_pass_permits_confirmation_only(self):
        result = development_gates(*evidence())
        self.assertTrue(result['passed'])
        self.assertFalse(result['promotion_eligible'])
        self.assertTrue(result['fresh_validation_required'])

    def test_low_median_cannot_hide_missing_near_targets(self):
        ref, candidate = evidence()
        candidate['free_running']['position_within_64px'] = 1
        self.assertFalse(development_gates(ref,candidate)['passed'])

    def test_one_combat_kind_cannot_substitute_for_both(self):
        ref, candidate = evidence()
        candidate['combat_actor_kind']['attack']['actor_kind_correct'] = 0
        self.assertFalse(development_gates(ref,candidate)['passed'])

    def test_large_actor_groups_cannot_game_recall(self):
        ref, candidate = evidence()
        candidate['decoded_actor_sets'].update(known_actor_true_positives=100,
            known_actor_false_positives=200,known_actor_false_negatives=0)
        self.assertFalse(development_gates(ref,candidate)['passed'])

    def test_total_improvement_cannot_hide_matchup_regression(self):
        ref, candidate = evidence()
        candidate['by_matchup']['PvT']['full_signature_correct'] = 0
        self.assertFalse(development_gates(ref,candidate)['passed'])

    def test_missing_combat_evidence_cannot_pass(self):
        ref,candidate=evidence()
        del candidate['combat_actor_kind']['attack']
        self.assertFalse(development_gates(ref,candidate)['passed'])

    def test_incompatible_denominators_rejected(self):
        ref,candidate=evidence()
        candidate['free_running']['slot_commands']=99
        with self.assertRaisesRegex(ValueError,'denominators'):
            development_gates(ref,candidate)


if __name__ == '__main__':
    unittest.main()
