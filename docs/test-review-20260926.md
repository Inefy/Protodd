# Production tests and Reaver prerequisite correction

## Results

- All **37** configured development test suites pass (37.61 seconds).
- All **four** targeted Win32 Release checks pass: core, available composition,
  Reaver prerequisite scenarios, and the BWAPI catalogue comparison.
- The first broad run passed 32/33 suites; its model suite used system Python
  without PyTorch. Configuring `build/dev` with the existing
  `build/model-venv/Scripts/python.exe` resolved it and enabled three additional
  training test suites. No packages were installed.
- The independent Reaver scenario failed four expectations before the fix.
  The expanded BWAPI comparison also failed specifically on Reaver prerequisites.
  Both pass after the correction.

Before/after logs, source copies and hashes are under
`build/test-review-20260926`. Win32 test binaries are under
`build/test-review-20260926-win32`.

## Confirmed bug and correction

`unitPrerequisites(reaver)` listed only Robotics Facility. BWAPI's bundled
`UnitType::requiredUnits()` also requires Robotics Support Bay. Consequently,
the planner could label Reaver training executable, reserve 200 minerals and
100 gas, and leave insufficient money for its missing 150-mineral/100-gas tech
building. The bridge correctly refused the impossible train command, but that
did not repair the planner's reservation.

The catalogue now includes both prerequisites. A missing Bay is funded first;
an early Bay warp-in leaves resources available to current production; near
completion can reserve a Reaver without issuing it; a completed Bay permits
training. Composition reinforcement uses the same corrected tech contract.
The regression covers those states and loss of the Robotics Facility.

The existing Win32 catalogue test now compares every supported directly trained
Protoss unit's prerequisite set with BWAPI, in addition to existing price,
supply and build-time checks across 28 Protoss catalogue entries. Reaver was the
only training prerequisite mismatch found. Building placement/power and Archon
merges use separate execution paths and are not claimed to be covered by the
new prerequisite comparison.

## Archived gameplay evidence

The read-only `review_reaver_logs.py` and `reaver-log-review.json` in the review
directory preserve log hashes and line references for the original baseline
and both completed reference arms of the Pylon follow-up.

In the original PvP baseline, game 0 logged 23 executable Reaver records before
Bay completion, beginning at frame 8674. The Bay started at 10627 and completed
at 11148. Four records were funded; rejected train attempts were observed.
Game 9 logged four executable Reaver records during Bay construction
(7873–8233; completion 8384). These are deduplicated diagnostic records, not a
count of every planning pass. They confirm exposure to the mismatch; they do
not establish that correcting it would reverse either loss.

## Evaluation boundary and next priority

The 12-game available-composition comparison remains owned by
`scripts/continue-available-composition.ps1`. Its frozen source/DLLs and gates
were not changed by this correction. The repeated reference arm is currently
running; the first reference arm completed four healthy losses. Do not launch
competing games or attribute those outcomes to this Reaver correction.

The next useful live check is whether correct tech funding produces an earlier
completed, armed Reaver against the observed ranged pressure while preserving
the Gateway screen. Run it only after the current comparison is reviewed, with
an isolated reference and candidate differing solely in this prerequisite fix.
The current result establishes planner/engine agreement and reservation
correctness, not improved win rate. Hourly automation remains paused and no
tournament package was replaced.
