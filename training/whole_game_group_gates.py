"""Predeclared bounded development criteria, distinct from promotion gates."""
DEVELOPMENT_GATES = dict(signature_min=0.05, signature_reference_multiplier=2.0,
    position64_min=0.10, position_reference_multiplier=1.5,
    combat_actor_kind_min=0.10, actor_precision_min=0.50, actor_recall_min=0.30,
    observed_fields_min=0.03, non_right_click_no_regression=True,
    position_median_improves=True, each_matchup_signatures_no_regression=True)


def development_gates(reference, candidate):
    r, c = reference['free_running'], candidate['free_running']
    if any(r[k] != c[k] for k in ('slot_commands','position_known')):
        raise ValueError('reference and candidate denominators differ')
    def fraction(row, num, den):
        return row.get(num, 0) / max(1, row.get(den, 0))
    sets = candidate['decoded_actor_sets']
    tp, fp, fn = (sets.get(k, 0) for k in ('known_actor_true_positives',
        'known_actor_false_positives', 'known_actor_false_negatives'))
    semantic = candidate['observed_field_audit_v2']
    measured = dict(signatures=fraction(c,'full_signature_correct','slot_commands'),
        reference_signatures=fraction(r,'full_signature_correct','slot_commands'),
        positions=fraction(c,'position_within_64px','position_known'),
        reference_positions=fraction(r,'position_within_64px','position_known'),
        actor_precision=tp/max(1,tp+fp), actor_recall=tp/max(1,tp+fn),
        observed_fields=fraction(semantic,'all_observed_fields_correct','commands'))
    checks = dict(signatures=measured['signatures'] >= max(.05,2*measured['reference_signatures']),
        positions=measured['positions'] >= max(.10,1.5*measured['reference_positions']),
        actor_precision=measured['actor_precision'] >= .5,
        actor_recall=measured['actor_recall'] >= .3,
        observed_fields=measured['observed_fields'] >= .03,
        non_right_click=c['non_right_click_correct'] >= r['non_right_click_correct'],
        position_median=(c['position_median_error_px'] is not None and
            (r['position_median_error_px'] is None or c['position_median_error_px'] < r['position_median_error_px'])))
    for kind in ('attack','attack_move'):
        row = candidate['combat_actor_kind'].get(kind, dict(samples=0, actor_kind_correct=0))
        measured[kind] = row['actor_kind_correct'] / max(1,row['samples'])
        checks[kind] = row['samples'] > 0 and measured[kind] >= .1
    for matchup in ('PvP','PvT','PvZ'):
        checks[matchup] = candidate['by_matchup'][matchup]['full_signature_correct'] >= reference['by_matchup'][matchup]['full_signature_correct']
    return dict(passed=all(checks.values()), checks=checks, measured=measured,
                promotion_eligible=False, fresh_validation_required=True)
