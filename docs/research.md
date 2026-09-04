# Open-source bot research notes

This document records the design ideas evaluated while hardening Astra. The
implementation remains original: repositories with restrictive, unclear, or
copyleft terms were used only to study architecture and testing practices.

## Projects reviewed

- [BWAPI](https://github.com/bwapi/bwapi) defines the legal observation and
  command boundary used by the tournament adapter.
- [UAlbertaBot](https://github.com/davechurchill/ualbertabot) provides the
  concrete opponent and its deterministic race-specific rush configurations.
- [PurpleWave](https://github.com/dgant/PurpleWave) demonstrates persistent
  strategic state, macro scheduling, and squad-level tactical decomposition.
- [McRave](https://github.com/Cmccrave/McRave) demonstrates a deliberately
  refined opening set, evidence-driven reactions, and producer-aware unit
  selection.
- [CommandCenter](https://github.com/davechurchill/commandcenter) reinforces a
  clean manager boundary and reproducible competition-bot structure.
- [StardustDevEnvironment](https://github.com/bmnielsen/StardustDevEnvironment)
  demonstrates scenario tests and full-game regression infrastructure.
- [Stardust](https://github.com/bmnielsen/Stardust),
  [Locutus](https://github.com/bmnielsen/Locutus),
  [Ecgberht](https://github.com/Jabbo16/Ecgberht), and
  [Tscmoo](https://github.com/tscmoo/tsc-bwai-merge) were consulted only for
  high-level concepts where their current licensing is not suitable for direct
  reuse in this MIT tournament entry.

## Ideas translated into Astra

The useful common pattern was not a single build order. Strong bots keep
strategic intent persistent, react to evidence before units arrive, allocate
production per available producer, divide armies by local objective, and test
both isolated scenarios and complete games.

Astra applies those ideas through its own data model and algorithms:

- first-seen tech timing, motion, and production-capacity inference;
- a strategic director that enters defense immediately and releases it only
  after a clear regrouping window;
- held resource reservations distinct from executable commands;
- parallel filling of genuinely idle producers;
- connected local squads, defensive screening, melee target leashes, exact
  volley allocation, and tactical Psionic Storm placement;
- close Nexus-anchored static defense against confirmed early Zerg pressure;
- deterministic core scenarios plus direct full-game BWAPI validation.

## Validation rule

The September 2026 audit also imported the official
[AIIDE 2025 BananaBrain package](https://davechurchill.ca/starcraft/aiide/results/2025/bots/)
as a local BWAPI 4.4.0 opponent. Its binary, configuration, pretraining data,
and source remain in the ignored benchmark vault. None of its implementation
is incorporated into Astra or its submission archive.

No behavior is accepted solely because it looks plausible in source. It must
pass portable regression tests, a strict 32-bit Release/BWAPI build, and direct
games where the live trace confirms milestone timing, production occupancy,
worker survival, and terminal outcome.
