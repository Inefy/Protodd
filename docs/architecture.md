# Astra architecture

Astra is a Protoss tournament bot with a deterministic, event-driven core. It
separates game observation from decision making so that strategic and tactical
logic can be replayed and regression-tested without running StarCraft.

## Frame pipeline

1. **Observe** converts visible BWAPI objects and remembered enemies into an
   immutable `GameState` snapshot.
2. **Infer** updates opponent opening probabilities, hidden-base likelihoods,
   threat fields, and enemy army estimates.
3. **Plan** reconciles the active opening with economy, production, technology,
   defense, and expansion goals. Goals reserve resources rather than issuing
   commands directly.
4. **Allocate** assigns workers, scouts, defenders, combat squads, and detectors.
5. **Act** produces deduplicated commands. The command bus arbitrates by urgency,
   applies a fair hard budget in very large battles, and the BWAPI bridge rejects
   invalid actions.
6. **Measure** records decisions and outcomes for replay-driven tuning.

Between games, Astra stores only aggregate win/loss counts by opponent, map,
and opening style. A deterministic UCB selector tries untested styles and then
balances observed win rate against uncertainty. It never reads a replay or
opponent file during a live game.

## Design rules

- No perfect-information flags or tournament-hostile behavior.
- No manager owns a BWAPI object. Stable IDs cross the adapter boundary.
- Every behavior has deterministic tie-breaking and a CPU budget.
- Urgent reactions run every frame; macro work is staggered across frames.
- Resource commitments include queued units and buildings under construction.
- Mobile enemy influence decays continuously after vision is lost; remembered
  buildings remain authoritative until their tile is seen empty.
- Resource bases use canonical depot tiles and distinct mineral-line centroids;
  ordinary construction is rejected when it would obstruct worker travel lanes.
- A safe fallback remains playable if terrain analysis or a subsystem fails.

## Implemented modules

| Module | Responsibility |
|---|---|
| `OpponentModel` | Base-anchored Bayesian opening, rush, proxy and capability inference |
| `InfluenceMap` | Ground/air threat, detection, mobility, and strategic value |
| `StrategyEngine` | Matchup plans, transitions, counter production, attack timing |
| `MacroPlanner` | Goal reconciliation, reservations, production and expansion |
| `WorkerManager` | Saturation, gas, transfer, construction and bounded threat-specific militia |
| `SquadPlanner` | Local connected armies, base defense, harassment, objectives and detector escorts |
| `CombatEvaluator` | Fast local fight estimate with uncertainty penalties |
| `TacticalController` | Volley allocation, kiting, surrounds, caster screening and cloak preservation |
| `BwapiBridge` | Legal observations, canonical base geometry, safe construction, upgrades and area-spell coordination |
| `CommandBus` | Legal command validation, deduplication, arbitration and throttling |
| `OpponentHistory` | Tournament-safe cross-game opening exploration and exploitation |

## Runtime constraints

The tournament DLL targets 32-bit StarCraft 1.16.1 and BWAPI 4.4.0. The core is
portable C++20. Expensive managers run on staggered cadences; combat commands
are capped at 96 per update and rotate fairly among equal-priority units.
