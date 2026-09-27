# Combat prediction and movement improvements — 26 September 2026

## Result

The bot now predicts individual weapon cooldowns without repeated rounding,
consumes suicide attackers after one impact, accounts for immediate worker
defenders in live squads, ignores unavailable pursuers, and checks terrain
before spreading against splash damage.

All **39 development test suites** pass. The new suite has **55 checks**, and
the Win32 Release core and combat suites both pass. The compiled playing DLL
completed the existing 200-supply engine fixture with **1,202 callbacks**,
**9.35 ms p99**, **14.698 ms maximum**, and zero caught or logging errors.
This establishes corrected behavior and bounded runtime in the tested cases.
It does not establish a win-rate improvement.

## Changes

| Area | Previous behavior | Corrected behavior |
| --- | --- | --- |
| Weapon timing | A six-frame simulation tick repeatedly rounded an eight-frame cooldown to twelve frames, and a fifteen-frame cooldown to eighteen. | Advance to the next actual cooldown or estimated contact event. Preserve initial reload delays and resolve both armies' same-frame volleys together. |
| Suicide attacks | Scourge, Spider Mines and Infested Terrans could survive their impact and fire repeatedly. Their one-frame cooldown also inflated the fast power estimate. | Consume each attacker after its first impact. Value its payload once over the estimation horizon instead of treating it as sustained one-frame fire. |
| Worker defense | Workers contributed no combat power or simulated attacks, and the live adapter's army list filtered them out before squad evaluation. | Admit visible, available workers within their weapon range plus 32 pixels, or pursuing a squad member within 160 pixels. Check known terrain separation. Add them after squad allocation so distant mining workers cannot absorb defensive reserves. |
| Reload kiting | Loaded passengers, hallucinations and unpowered defenses could make ranged units move away during reload. | Require an available, compatible attacker before declaring a pursuer. |
| Splash spacing | Loaded, hallucinated, invincible or empty Reavers could scatter Dragoons. | Require an available attacker with ammunition and a compatible weapon. Respect minimum weapon range. |
| Terrain | Splash spacing could choose a destination across blocked terrain. A later global path check could allow a long detour around the cliff. | Share the local segment check already used by retreat and kiting. Try a reachable alternative while retaining attack-frame and ready-volley protection. |

The combat simulator still uses its existing fixed-position approach model,
96-unit limit per side and fourteen-second horizon. This change makes timing
exact within that model; it does not turn it into a full StarCraft simulator.
Worker admission uses legal visible observations. It does not infer mining
workers behind fog or change ownership of our own workers.

The weapon cooldown contract is documented by
[BWAPI 4.4's WeaponType reference](https://bwapi.github.io/class_b_w_a_p_i_1_1_weapon_type.html).
Weapon identities are also available in the bundled BWAPI headers.

## Regression evidence

`build/combat-strength-20260926` contains the pre-edit sources, regression logs,
benchmark executable, final DLL, runtime review and SHA-256 source/evidence pins.

- The initial new suite produced **28 failed assertions** against the original
  implementation. These cover multiple cooldown/delay combinations, suicide
  impacts, workers, unavailable threats and blocked spacing.
- An additional live-path regression produced **two failures** before the squad
  integration change. This caught the distinction between accepting workers in
  the evaluator and actually supplying them from the playing bot.
- The final 55 checks also cover simultaneous lethal volleys, reload versus
  approach timing, unreachable contact, observation-order invariance, passive
  encounters, distant mining workers, fog, air squads, real pursuers and a cliff
  with a valid distant detour.
- Existing core, hidden-health, Storm, ammunition and production tests pass.

## Runtime evidence

The deterministic benchmark uses 96 Dragoons versus 96 Zerglings, staggered
initial cooldowns, five warm-up evaluations and fifty measured evaluations.
Both binaries are Win32 Release builds. Three alternating runs gave:

| Run | Original median | Candidate median |
| --- | ---: | ---: |
| 1 | 2.2917 ms | 1.7173 ms |
| 2 | 2.2841 ms | 1.7395 ms |
| 3 | 2.2977 ms | 1.7171 ms |

The median of these medians is **25.1% lower**. The simulator reuses damage
buffers and skips directly to eligible events. The checksums differ because
the predicted outcomes correctly change. These measurements cover this fixture
and local hardware, not every possible army composition.

Full engine evidence is in
`build/audit-validation-20260926/load-combat-accuracy-final`. Its wrapper times
the entire actual playing callback, including diagnostics. The fixture starts
with 200 supply, 125 own entities and 96 visible enemy entities. Both the initial
and final runs completed; only the final run validates the worker integration.
The native suite was also running during part of the final fixture.

Final DLL: `build/combat-strength-20260926/Protodd.final.dll`.
SHA-256: `8a63c413c95ebda48d668a8d1ce24477ffe9f4a5610cff076a7ed2ec5505acb6`.

The fixture intentionally exits at frame 1201. Its generic bot `END,loss` record
is not a competitive match outcome. No callback reached 42 ms or 55 ms.
No competitive campaign or tournament promotion was performed. Existing audit
comparison inputs remain separate, and learned control remains disabled.

## Reproduce

```powershell
cmake --build build/dev --parallel 2
ctest --test-dir build/dev --output-on-failure
cmake --build build/test-review-20260926-win32 --config Release --target protodd_combat_accuracy_tests protodd_tests protodd_bwapi --parallel 2
ctest --test-dir build/test-review-20260926-win32 -C Release -R 'protodd_(core|combat_accuracy)_tests' --output-on-failure
build/test-review-20260926-win32/tests/Release/protodd_combat_accuracy_tests.exe --benchmark
python build/combat-strength-20260926/review.py
```

The last command verifies the preserved local fixture and writes its runtime
review. It requires the Git-ignored engine evidence. Match-strength evaluation
remains a separate step using frozen reference and candidate binaries.
