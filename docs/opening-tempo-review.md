# Opening tempo and test isolation — September 7, 2026 UTC

This follows the [containment investigation](containment-review.md). Its final
v7 binary lost both BananaBrain games. The next experiment changes one opening
checkpoint, rather than combining additional combat and macro features.

## Reproduced execution error

In `containment-v7-destination-42.log`, the quiet opening requested one Zealot,
but composition spending trained a second during Core construction and queued
a third by frame 4,560. The Core completed at 4,776. Robotics and the second
Gateway then outranked the first Dragoon's resource request. The first Dragoon
completed at 6,144. This is a production-order problem before the first enemy
contact at 7,464; changing the later attack threshold cannot repair it.

The candidate keeps the explicit bodyguard and first two Dragoons as the
opening checkpoint. Before those Dragoons are committed, the generic
composition filler cannot insert extra Zealots. The first ranged goal outranks
optional Robotics spending. Observed pressure still selects the separate rush
response. No combat, placement, expansion, or enemy-specific decision changes
are part of this candidate.

## A previously uncontrolled test condition

BananaBrain's `Source/Results.cpp` reads its prepared AI results, then runtime
`read/Results_<name>.txt`, falling back to `write/Results_<name>.txt`. Its
configuration also accepts an explicit `PvP_opening`. The direct runner had
cleared Protodd's learning while leaving the opponent's runtime history
available. Thus older `learning_preserved=false` records do **not** establish
that opponent learning was reset. In particular, prior intermediate games
are not controlled A/B evidence.

The runner now archives both opponent runtime data directories by default,
preserving their files under the match's `-opponent-before` directory. It
supports a runtime-only BananaBrain opening override, records the effective
configuration hash and learning-reset condition, and archives opponent result
diagnostics. Installed opponent files remain unchanged. `-PreserveLearning`
retains both sides' runtime histories. Reports separate configurations,
opening overrides, and learning conditions into different result segments.

The experiment fixes BananaBrain to its existing `PvP_nzcore` opening on
Destination, requested seed 42, with fresh runtime histories. This isolates
the opening choice and starting learning condition; it does not promise
bit-for-bit deterministic unit movement or establish broad tournament strength.

## Validation

The regression passes the full strategic plan through macro reconciliation:
no extra Zealot may occupy the Gateway during quiet Core construction, and
the first available ranged unit must receive resources before Robotics.
Observed rush behavior is checked separately. Release CTest and the strict
verifier pass. Report tests additionally prevent pooling different opponent
opening and learning conditions.

Frozen candidate: `build/tempo-audit/first-ranged-screen.dll`.
Frozen baseline: `build/containment-audit/candidate-v7.dll`.
Authoritative results: `build/direct-logs/tempo-*.json`.

Candidate SHA-256:
`D54168412D058381E603B81A60D3841CD0D4C76EC0F91301A41744233B1BA8AD`.

| Build | Result | First Core / Dragoon / range frames | Peak Probes / army / Nexuses |
| --- | --- | --- | --- |
| Frozen v7 baseline | Loss, frame 15,316 | 4,896 / 6,168 / 8,256 | 22 / 16 / 1 |
| First ranged screen | Loss, frame 12,712 | 4,728 / 5,472 / 7,968 | 22 / 8 / 1 |

BananaBrain's own fresh result record confirms `PvP_nzcore` for the baseline;
the copied evidence is `build/tempo-audit/baseline-opponent-results.txt`.

The candidate also observed `PvP_nzcore`. Its first Dragoon arrived 696 frames
earlier, but its base was breached at frame 9,864, compared with 11,112 in the
baseline. Earlier ranged production alone did not improve this match. The
subsequent [terrain defense pass](terrain-defense.md) addresses army composition
execution and the user's observations about defending chokepoints.
