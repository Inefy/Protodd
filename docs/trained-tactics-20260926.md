# First trained tactical scope: target selection

The trained Protodd experiment now has an opt-in target ranker. It chooses
among enemy units already admitted by `CombatEvaluator::selectTarget` after
visibility, detection, weapon compatibility, range, overkill, melee-locality
and immediate-shot checks. Engagement, retreat, kiting, spells, movement,
scouting and production remain under their current controllers. The default
ladder build has no learned target control.

`TacticalTargetModel` is a small CPU scorer with five replay-observable numeric
features and 256 optional unit-type biases. `training.tactical_target_fit`
uses verified v32d **train** shards only. It joins a confirmed own command to
the observation immediately before that command, requires one confirmed
Protoss combat actor and a visible enemy target, and ranks at most 32 visible
enemy candidates within 640 pixels. Distinct train-split games are used for
fitting and development; release validation and final-test shards are not read.
The label is immediate command-state transition, not proof that a shot landed
or that the target choice helped win.

## Bounded fits

Both fits used 24 fit and eight development games per matchup, selected by a
stable game-id hash. Their artifacts are in ignored `artifacts/replay-learning`.

| Variant | Fit examples | Development examples | Development top-1 | Nearest visible target |
| --- | ---: | ---: | ---: | ---: |
| Attack commands, type biases | 232 | 91 | 40.7% | 52.7% |
| Attack plus enemy right-click, no type biases | 818 | 297 | 39.4% | 36.7% |

The first variant overfit and failed its simple comparator. The second is a
small development gain on a broader but still noisy imitation label. Examples
within one game are correlated, and its candidate set does not reproduce every
live combat legality decision. **Neither fit is qualified as a strength gain or
for tournament control.** The v2 model SHA-256 is
`0ad5d4ed9041ffabd1082cce910e9c4af688838749161e7621fad150664550cf`.
The full game list, release identity, trainer hash, metrics and model hash are
in each fit's `report.json`.

## Runtime boundary

The tournament option `PROTODD_TACTICAL_LOCAL_EVALUATION` defaults to `OFF`.
An isolated local evaluation build with that option `ON` reads
`bwapi-data/read/TacticalTarget-weights.bin`. It takes target control only
when `bwapi-data/read/TacticalTarget-mode.txt` starts with `local-target`.
Missing, malformed or unsupported weights leave the existing target selector
in charge. This marker is for diagnostic games with frozen inputs, not a
promotion receipt. The older six-slot whole-game model remains unpromoted;
this target scorer is the first narrow tactical scope in the newer hybrid
direction described by [the strength-first plan](strength-first-plan.md).

## Checks and next gate

- Python extraction tests verify the causal observation join and filter out
  non-enemy right-click targets.
- Native tests verify weight loading, feature values, the untouched default
  choice, and legality/range/overkill constraints on learned choices.
- The experimental and default Release/Win32 DLLs compile. The exported v2
  model's two probe scores match Python and Win32 exactly to displayed precision.
- The local 200-supply engine fixture loaded the exact v2 weights with
  `local-target` control. It scored 258,868 target candidates in 1,202 full
  callbacks; p99 was 11.144 ms, maximum 17.770 ms, with zero callbacks over
  42 ms and zero caught errors. The frozen fixture, DLL and weights are under
  ignored `build/audit-validation-20260926/load-tactical-target-v2-instrumented`.
- All 41 development suites pass, including the new C++ and Python tactical
  tests.

Before any strength claim, measure the difference from the heuristic on the
same legal candidate sets, then run a bounded local game screen and a matched
reference/candidate comparison. Keep the v2 fit in diagnostic status unless
those checks show a meaningful benefit. The load fixture exits at frame 1201;
its `END,loss` is not a competitive match result.

The first full-game local target-control screen is frozen under
`build/trained-target-screen-20260927`. Both arms receive the same v2 weights
and `local-target` mode file as pinned arena inputs. The heuristic DLL ignores
the mode; the candidate evaluation DLL can use the scorer only for legal
targets already admitted by the combat evaluator. Four matched PvZ games per
arm test activation, combat exposure, health, and outcomes. No tournament
promotion is allowed by this pilot. All eight matches were healthy. The trained
DLL loaded the exact v2 weights and scored 11,103, 60,485, 33,333 and 3,308
legal candidates across its four games. Its functional integration gate passed,
but both arms lost all four games. The model remains opt-in research code, not
a validated win-rate improvement. See the frozen `pilot-report.json` in that
directory.

The follow-up same-set diagnostic is frozen under
`build/target-disagreement-20260927`. In two healthy games against McRaveZ,
the trained scorer evaluated 35,795 legal candidates. Its selected target
disagreed with the heuristic in 652 of 3,581 comparisons (18.2%). None of
those disagreements crossed the broad worker/building versus combat categories
or abandoned an immediately threatening enemy under the diagnostic definition.
Both games were losses. This narrows the next inspection to within-category
choices and does not establish a strength gain. The trained scorer remains
opt-in and unpromoted.
