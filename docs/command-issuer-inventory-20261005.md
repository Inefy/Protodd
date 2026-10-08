# Gameplay command issuer inventory (5 October 2026)

This inventory follows gameplay orders to their BWAPI acceptance boundary. It records arbitration and budget ownership separately: a command accepted by BWAPI is an acknowledgment, while a later snapshot or lifecycle event is evidence of the effect.

## Protodd tournament module

`BwapiBridge::issue` in `src/bwapi/BwapiBridge.cpp` is the single `UnitInterface::issueCommand` boundary for Protodd. It captures the BWAPI result immediately, reports accepted and rejected attempts to `ProtoddModule::logAction` through `actionDiagnostic`, and tags each order with an issuer/source. The trace is written as `ACTION` rows. `reject` also records intents that fail before reaching BWAPI, so those cannot be confused with accepted engine commands.

| Issuer / owner | Entry path and command families | Arbitration and budget |
| --- | --- | --- |
| Main army, base defense, detector escorts, transports | `ProtoddModule::updateCombat` submits squad, transport and escort orders to `commands_`; `updateCombat` finalizes the combat bus and executes through `BwapiBridge::execute`. Includes move, attack, hold, load/unload and technology commands. | `CommandBus` priority, actor deduplication, active-order suppression and frame/latency windows apply. The caller supplies a per-frame command limit from `FrameBudget`; deferred counts are available from bus statistics. |
| Worker scouts | `ProtoddModule::updateScoutMicro` submits scout micro to `scoutCommands_`; `updateScouting` sends assigned worker-scout movement through `executeScouts`. | Micro bus is capped at one command per update. Assigned-scout orders use the bridge directly and are not charged to the combat bus. |
| Worker economy, evacuation and defense | `BwapiBridge::assignWorkers` issues mineral/gas gathering, mineral-walk escape, evacuation, cargo return and local worker-defense orders. | Worker allocation and eligibility are decided by `WorkerController`; orders bypass `CommandBus` and have no shared frame-wide command cap. Each outcome is still recorded at the bridge boundary. |
| Build and production macro | `BwapiBridge::execute(MacroAction)` issues builder preposition/build, training, research and upgrades. Strategy reasons are retained as the source tag. | Macro planning, reservations and producer legality choose the actions; the action path does not share the combat bus's command cap. Pending build leases and production feedback track later effects. |
| Maintenance | `BwapiBridge::runMaintenance` issues Scarab/Interceptor training, Stasis, Recall, Feedback and Archon fusion. | The bridge checks available/reserved resources and has category-local limits (for example, at most one Recall and one selected Stasis cast); it bypasses `CommandBus` and the combat command cap. |
| Learned production adapter | `ProtoddModule` calls `BwapiBridge::executeProduction`; `ProductionDemandAdapter` can issue training through the common bridge. | Adapter-specific permissions, resource reservations and actor leases apply. No global command budget is shared with combat or workers. |
| Whole-game controller | `BwapiBridge::executeWholeGame` submits learned actions with the `whole-game` source tag. | One live lease per actor suppresses conflicting learned orders; this path bypasses the combat bus. |
| Observer safety | The post-combat `observer-safety` phase asks `ScoutManager::protectObservers` for orders and executes them through the same bridge as combat. | It checks bridge active-order state, but executes after combat and is not charged to the combat bus's selected-command limit. This is a separate-priority path to watch for same-frame conflicts. |

The direct bridge call-site inventory is in `BwapiBridge.cpp`: `execute`, worker assignment/scouting, maintenance, macro execution, learned production and whole-game execution. There is no direct `UnitInterface::issueCommand` call elsewhere in the Protodd adapter.

## Follow-up: guarded acceptance boundaries (7 October 2026)

`tests/test_command_issuer_inventory.py` scans every production `.cpp` and `.hpp` file and fails if a new direct BWAPI `issueCommand` call appears outside the two recorded boundaries: `BwapiBridge::issue` and `RaceBotModule::issueCommand`. It also checks that each boundary retains its `ACTION` or `COMMAND` acceptance/error trace, and that learned production dispatches through the bridge. CMake registers this audit in the Python CTest group. This prevents a source-level bypass; it does not replace stock-engine traces for the remaining RaceBot and recovery issuers.

## Standalone RaceBot modules

`RaceBotModule` is a separate baseline used by the TerranTodd/ZergTodd module targets. It does not use `CommandBus` or `BwapiBridge`. All its direct BWAPI orders now pass through `RaceBotModule::issueCommand`, which writes a `COMMAND` row with frame, actor, issuer, command type, target/position, acceptance and BWAPI error to `bwapi-data/write/RaceBot.log`.

| Issuer / owner | Commands | Arbitration and budget |
| --- | --- | --- |
| Macro | Build, train and Zerg morph orders. | Macro returns after one accepted strategic action; one pending construction lease gates further builds. No shared command-count cap. |
| Construction recovery | Stop an abandoned walking builder; right-click to resume an unassigned Terran structure. | One active construction record; the worker eligibility checks gate retries. No shared command-count cap. |
| Workers | Local defense attack, return cargo, gas/mineral gather. | Worker selection limits active defenders and assigns one job per worker; no shared command-count cap. |
| Combat | Medic healing/escort, army attack/advance and Overlord return movement. | Readiness, attack-frame and destination checks suppress redundant orders; no shared command-count cap. |

## Current gaps

- Issuer/source and API acceptance are logged, but common actor ownership, urgency, deadlines and lease generations do not span all paths.
- `CommandBus` suppression windows and latency behavior now have the T014 engine-backed calibration; direct bridge issuers have separate retry rules.
- Worker, macro, maintenance, learned and observer-safety orders are not charged to one global bounded budget (T015).
- `ACTION` and `COMMAND` report API acceptance, not observed completion. The existing build and production feedback paths cover only selected effects; T013 remains open.
- Stock Protodd bridge traces now cover worker-mining, worker-defense, assigned-scout movement and whole-game actor-lease behavior, alongside combat, maintenance and Observer safety. The whole-game trace confirms an accepted learned Move, rejection of a conflicting native bridge order during its 24-frame lease, and acceptance of a native Move after expiry. The RaceBot smoke confirms accepted records across macro build, production and worker mining; RaceBot construction recovery, worker defense and combat still need case-specific traces.
