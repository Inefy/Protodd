import json

import pytest

from training.whole_game_cadence_compare import AUDIT_SCHEMA, compare


def _audit(path, kind, correct, frame=24):
    row = dict(game_id="game:" + "a" * 64, frame=frame, first_kind="right_click",
               first_action_lag=3, commands_in_window=1, actor_known=True,
               first_kind_correct=correct, joint_pair_correct=correct,
               actor_correct=False, predicted_kind=kind)
    report = dict(schema=AUDIT_SCHEMA, validation_identity_sha256="validation",
                  checkpoint_sha256=kind, games={"PvT": 1, "PvZ": 1, "PvP": 1},
                  overall={"windows": 5}, rows=[row])
    path.write_text(json.dumps(report))


def test_compare_requires_identical_deployable_windows(tmp_path):
    first, second = tmp_path / "a.json", tmp_path / "b.json"
    _audit(first, "right_click", True)
    _audit(second, "build", False)
    result = compare({"a": first, "b": second})
    assert result["majority_kind_correct"] == 1
    assert result["candidates"]["a"]["first_kind_top1"] == 1
    assert result["candidates"]["b"]["first_kind_top1"] == 0
    _audit(second, "build", False, frame=48)
    with pytest.raises(ValueError, match="same cadence windows"):
        compare({"a": first, "b": second})


def test_argument_metrics_need_complete_evidence_for_every_candidate(tmp_path):
    first, second = tmp_path / "a.json", tmp_path / "b.json"
    _audit(first, "right_click", True)
    _audit(second, "right_click", True)
    original = json.loads(first.read_text())
    richer = dict(original)
    richer["rows"] = [dict(original["rows"][0], target_mode_correct=True,
                           target_entity_known=False, target_entity_correct=False,
                           target_position_known=True, target_position_error_px=12,
                           unit_type_known=False, unit_type_correct=False,
                           full_signature_known=True, full_signature_correct=True)]
    first.write_text(json.dumps(richer))
    assert compare({"rich": first, "old": second})["argument_metrics_available"] is False
    second.write_text(json.dumps(richer))
    result = compare({"a": first, "b": second})
    assert result["argument_metrics_available"] is True
    assert result["candidates"]["a"]["target_position_within_64px"] == 1
    assert result["candidates"]["a"]["full_signature_top1"] == 1
