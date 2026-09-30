# UAlberta Terran retest - 30 September 2026

The unchanged final build from `c7c2453` achieved one clean win on each host
side against the local UAlberta Terran package. Three attempts were played:
two clean wins and one opponent crash excluded. This is a small development
diagnostic, not a Terran matchup strength estimate.

## Inputs

- Protodd played Protoss. DLL SHA-256:
  `7649D842F7CBB18479AE98325557FB1E7AB92438D9344DB800DFF0E363B9E262`.
- Local UABTerran DLL SHA-256:
  `A99DF20B634298D746114673596A1CE4D6A152D148CD086F8FA67BC803598183`.
  The installed configuration selects `Terran_MarineRush`.
- Destination map hash: `4e24f217d2fe4dbfa6799bc57f74d8dc939d425b`.
  Cap: 28,800 frames. Frozen modes, empty initial learned reads, and manager
  bot-file learning disabled, as in the [earlier diagnostic](uab-diagnostic-20260929.md).
- Both campaigns verified before launch and after completion. Original IDs,
  host sides and actual logged seeds were retained. The retry was a fresh
  campaign, not a replacement or overwrite of the failed attempt.
- No gameplay code or configuration changed during this retest. The existing
  build's 93 passing native suite executions were not rerun for this report.

## Results

| Attempt | Game / Seed | Protodd Host | Classification | Own Final Frame | Own Sampled Peak Army / Probes |
| --- | --- | --- | --- | --- | --- |
| Initial | 0 / 202609900 | Yes | Clean win | 25,391 | 56 / 44 |
| Initial | 3 / 202609903 | No | Excluded: opponent STARCRAFT_CRASH | 13,890 | 17 / 22 |
| Fresh retry | 3 / 202609903 | No | Clean win | 22,818 | 52 / 44 |

Both accepted pairs reported NORMAL, agreed on the winner, and ended below
the cap without crash or timeout flags. Enemy visible army peaks were 37 and
21; opponent scores were 29,039 and 15,940. These were active opponents.
Every own summary, including the excluded attempt, had zero caught errors
and zero logging errors. No frozen-empty policy Hold override was observed.

The old default's game 3 was a clean loss at frame 12,092. The new clean retry
won on the same seed and host side. Old game 0 was excluded for an opponent
crash, so it supplies no decisive baseline comparison.

The failed initial game 3 has an own NORMAL/win report, but the opponent's
report is STARCRAFT_CRASH at frame 13,680. It is not counted as a win. No crash
cause was established. Same-seed attempts have different observed timings;
this environment is not assumed to replay deterministically.

## Follow-Up Targets

1. Construction and supply recovery near a threatened rally. In excluded game
   3, states from frame 7,200 through 10,080 stayed at 64/66 supply with funded
   Pylon/Core goals and `build-no-builder`. The bank grew from 608 to 2,608
   minerals. Core construction finally began at 10,398. The rally was
   (1424,3696), away from the main at (2112,3824). A likely cause is the bridge's
   long-path threat filter rejecting builders for the forward rally; this is
   an inference, not an isolated fix. The near-cap Pylon does not use the
   fully-supply-blocked home-anchor fallback. Preserve route safety while
   testing a safe home construction fallback.
2. Expansion coverage. In the clean retry, Nexus 228 started at frame 11,123
   and was destroyed unfinished at 11,992. Its replacement started at 12,754
   and completed at 14,625. The bot recovered, but spending the first 400
   minerals on an exposed site remains avoidable risk.

The clean retry also had 156 supply-block summary samples and 310 high-bank
samples, with peak minerals 2,477. These counters are diagnostic observations,
not a causal attribution or proof of a general performance regression.

## Evidence

Initial run: `build/uab-terran-final-20260930/candidate/`.
Retry: `build/uab-terran-retry-20260930/candidate/`.
Both retain verified manifests, raw paired reports, frozen DLLs and archived
own logs. `build/review_income_screen.py` regenerates each diagnostic report.
`build/terran-review-20260930.json` pins the evidence and this document.

All results remain development-only: `training_ready=false` and
`strength_validated=false`. Owned Java runners were stopped after all reports
arrived and StarCraft exited. No test processes remain running.
