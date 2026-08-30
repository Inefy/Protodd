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
5. **Act** produces deduplicated commands. The BWAPI bridge rate-limits commands
   and rejects invalid or redundant actions.
6. **Measure** records decisions and outcomes for replay-driven tuning.

## Design rules

- No perfect-information flags or tournament-hostile behavior.
- No manager owns a BWAPI object. Stable IDs cross the adapter boundary.
- Every behavior has deterministic tie-breaking and a CPU budget.
- Urgent reactions run every frame; macro work is staggered across frames.
- Resource commitments include queued units and buildings under construction.
- Enemy memory decays by mobility and visibility, not by a fixed timeout.
- A safe fallback remains playable if terrain analysis or a subsystem fails.

## Planned modules

| Module | Responsibility |
|---|---|
| `OpponentModel` | Bayesian opening recognition and enemy capability estimates |
| `InfluenceMap` | Ground/air threat, detection, mobility, and strategic value |
| `StrategyEngine` | Matchup plans, transitions, counter production, attack timing |
| `MacroPlanner` | Goal reconciliation, reservations, production and expansion |
| `WorkerManager` | Saturation, gas policy, transfer, construction, worker defense |
| `SquadManager` | Role assignment, clustering, objectives and reinforcement |
| `CombatEvaluator` | Fast local fight estimate with uncertainty penalties |
| `MicroController` | Targeting, kiting, formations, spells, transport and detection |
| `CommandBus` | Legal command validation, deduplication, arbitration and throttling |

## Runtime constraints

The tournament DLL targets 32-bit StarCraft 1.16.1 and BWAPI 4.4.0. The core is
portable C++20. In a release build, expensive work is amortized and a frame
budget governor degrades gracefully from full search to cached decisions.

