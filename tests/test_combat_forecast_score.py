import importlib.util
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location(
    "score_combat_forecasts", ROOT / "tools" / "score_combat_forecasts.py")
SCORER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SCORER)


def row(event_id="1", **overrides):
    data = {
        "gameId": "987", "seed": "31", "mapHash": "42", "opponentId": "57",
        "opponentRace": "2", "pvtStrategy": "0", "key": "9",
        "start": "100", "end": "436", "forecastFrame": "100", "actionFrame": "100",
        "outcomeFrame": "436", "expectedHorizonFrames": "336", "observations": "4",
        "predictedLoss": "20", "predictedSurvival": "0.75",
        "proposedAction": "0", "finalAction": "0", "forecastProposedAction": "0",
        "forecastFinalAction": "0", "simulation": "1", "outcomeObserved": "1",
        "resolved": "0", "censor": "horizon_reached", "initialComplete": "1",
        "forecastRosterComplete": "1",
        "simulationRosterCoverage": "unknown",
        "calibrationStatus": "uncomparable_simulation_power_vs_hp_shields",
        "friendlyLoss": "25", "enemyLoss": "100",
        "initial": "F:1:1:100:1;E:2:2:100:1",
        "latest": "F:1:75:1;E:2:2:0:1",
        "forecastRoster": "F:1:1:100:1;E:2:2:100:1",
        "outcomeRoster": "F:1:1:75:1;E:2:2:0:1",
    }
    data.update(overrides)
    return "COMBAT_FORECAST," + event_id + "," + ",".join(
        f"{key}={value}" for key, value in data.items())


class CombatForecastPairAuditTests(unittest.TestCase):
    def test_same_observed_roster_and_exact_horizon_remain_unverified_diagnostics(self):
        result = SCORER.audit_lines([row()])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 1)
        self.assertEqual(result["pair_rejections"], {})
        self.assertEqual(result["simulation_roster_coverage"], "unknown")
        self.assertIsNone(result["calibration_metrics"])
        self.assertTrue(result["calibration_metrics_suppressed"])

    def test_unknown_simulation_coverage_rejects_any_full_coverage_claim(self):
        result = SCORER.audit_lines([row(simulationRosterCoverage="full")])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 0)
        self.assertEqual(result["pair_rejections"],
                         {"unsupported_simulation_roster_coverage_claim": 1})

    def test_mismatched_rosters_are_rejected(self):
        result = SCORER.audit_lines([row(outcomeRoster="F:1:1:75:1;E:3:2:0:1")])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 0)
        self.assertEqual(result["pair_rejections"], {"unit_roster_mismatch": 1})

    def test_mismatched_frames_are_rejected(self):
        result = SCORER.audit_lines([row(outcomeFrame="437")])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 0)
        self.assertEqual(result["pair_rejections"], {"frame_mismatch": 1})

    def test_unknown_units_and_incomplete_forecasts_are_rejected(self):
        result = SCORER.audit_lines([
            row("1", forecastRosterComplete="0"),
            row("2", outcomeRoster="F:1:1:75:1;E:2:2:0:0"),
        ])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 0)
        self.assertEqual(result["pair_rejections"].get("incomplete_known_units"), 2)

    def test_lost_vision_reinforcement_retreat_and_match_end_are_censored(self):
        result = SCORER.audit_lines([
            row("1", censor="lost_contact", outcomeObserved="0"),
            row("2", censor="reinforcement_or_roster_change", outcomeObserved="0"),
            row("3", censor="retreat", outcomeObserved="0"),
            row("4", censor="match_end", outcomeObserved="0"),
        ])
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 0)
        self.assertEqual(len(result["pair_rejections"]), 4)

    def test_early_outcome_or_missing_forecast_is_rejected(self):
        result = SCORER.audit_lines([
            row("1", censor="early_resolution_before_horizon", outcomeObserved="0"),
            row("2", simulation="0", forecastFrame="-1"),
        ])
        self.assertEqual(result["pair_rejections"].get("censored:early_resolution_before_horizon"), 1)
        self.assertEqual(result["pair_rejections"].get("no_forecast"), 1)

    def test_censor_counts_include_all_elimination_modes_and_note_selection_bias(self):
        result = SCORER.audit_lines([
            row("1", censor="early_resolution_before_horizon", outcomeObserved="0"),
            row("2", simulation="0", forecastFrame="-1", censor="friendly_eliminated"),
            row("3", simulation="0", forecastFrame="-1", censor="enemy_eliminated"),
            row("4", censor="lost_contact", outcomeObserved="0"),
        ])
        self.assertEqual(result["complete_elimination_records"], 3)
        self.assertEqual(result["censor_reason_counts"], {
            "early_resolution_before_horizon": 1, "friendly_eliminated": 1,
            "enemy_eliminated": 1, "lost_contact": 1,
        })
        self.assertIn("survivorship-biased", result["selection_bias_note"])

    def test_duplicate_rows_dedupe_and_repeated_seed_game_ids_stay_distinct(self):
        result = SCORER.audit_lines([row(), row(), row("1", gameId="988")])
        self.assertEqual(result["unique_records"], 2)
        self.assertEqual(result["duplicate_records"], 1)
        self.assertEqual(result["observationally_aligned_unverified_simulation_count"], 2)

    def test_endflush_reconciles_records_and_flags_missing_summary(self):
        lines = [row(), row("2", censor="retreat", outcomeObserved="0"),
                 "COMBAT_FORECAST_SUMMARY,gameId=987,enabled=1,created=2,flushed=2,censored=1,incomplete=0,duplicates=0,droppedStartAttempts=0,writeErrors=0"]
        result = SCORER.audit_lines(lines)
        self.assertTrue(result["summary_matches"])
        self.assertTrue(result["flush_matches"])
        interrupted = SCORER.audit_lines([row()])
        self.assertFalse(interrupted["summary_matches"])
        self.assertFalse(interrupted["flush_matches"])
        self.assertTrue(SCORER.audit_lines([])["summary_matches"])

    def test_no_accuracy_metric_is_emitted_for_uncomparable_units(self):
        result = SCORER.audit_lines([row()])
        self.assertNotIn("mae", result)
        self.assertNotIn("brier", result)
        self.assertEqual(result["calibration_status"],
                         "uncomparable_simulation_power_vs_hp_shields")

    def test_disabled_default_and_history_noninterference_are_explicit(self):
        header = (ROOT / "include" / "protodd" / "CombatForecastAudit.hpp").read_text()
        module = (ROOT / "src" / "bwapi" / "ProtoddModule.cpp").read_text()
        self.assertIn("bool enabled_{};", header)
        self.assertIn('readFile("bwapi-data/read/Combat-forecast-audit.txt").starts_with("on")', module)
        self.assertNotIn("forecastAudit_", (ROOT / "src" / "core" / "Learning.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
