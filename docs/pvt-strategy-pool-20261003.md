# PvT mech strategy pool - 3 October 2026

## Source and scope

This R3 working copy starts from the exact tracked Git tree for baseline
`11f263ee38c0325799d35cd88a9d589229e3bc42`, materialized using a temporary
copy of the verified `clearance-publication` repository metadata. Every one of
the 563 committed file blobs was compared against the materialized snapshot;
all matched. Only the PvT strategy pool, its selector/logging, PvT regression
scenarios, and this documentation are ported here. PvZ readiness production
changes and the standalone PvZ Lair-insurance fixture are excluded. The
original repository, frozen publication checkout, HYBRIDON build, and
production weights are not modified. This remains source-only: it does not
alter learned weights, train a policy, or publish a candidate.

Future builds must keep HYBRIDON enabled and use the frozen production weights
SHA-256 `5fac1e322748d78c878a7f0043e3797a7ca3ff5e3a7b7a84d1233bcb33532d16`.
The strategy file is the only intended per-arm behavioral difference.

`PvTStrategyId` gives each evaluation arm a stable identifier. Write one of
these values to `bwapi-data/read/PvT-strategy.txt` before a local match:

| ID | Policy |
| --- | --- |
| `standard` | Existing PvT plan; default when the selector file is absent or invalid. |
| `safe-2gateway-range-observer` | Request two Gateways and the existing Dragoon range/Observer tech chain. Keep the expansion goal at one base until two Gateways, the Core, two completed Dragoons, completed Singularity Charge, and one completed Observer are present. With any recently observed Factory, Vulture, Tank, or mine, require mobile detection, raise the attack threshold, and avoid pressure until an Observer is complete. |
| `economic-1gateway-observer` | Activate the one-Gateway economic transition only after two positively observed Command Centers and a quiet home threat state. Quiet means the current rush, combat contact, approach, immediate-ground, and breach checks pass; proxy plus static-contain is at most 0.34 and aggression is at most 0.62. Hold one Gateway until our second Nexus exists; then use the existing Robotics/Observatory/Observer goals. A hidden or unscouted natural is unknown, not evidence that Terran skipped expansion. |

The ID is fixed at startup for the whole game. Scouts change each line's
transitions; they do not trigger online strategy learning or switch the
evaluation arm. The log records the selected ID, opponent alias, resolved
Terran race, recent Factory/Command Center/Vulture/Tank/mine cues, and completed
Observer count. Existing opponent learning remains keyed by opponent alias;
this strategy selector writes no outcome data.

## Existing supported behavior

Protodd already builds Dragoons and researches Singularity Charge, requests
Observers against Mines and multiple Tanks, transitions Zealots/Leg Enhancements
against scouted mech, and uses local Terran-front target safeguards in squad
control. The first patch reuses those production and control paths. It does not
add Carriers, Shuttle/Reaver control, or a new mine-clearing maneuver.

The strategy commitments are not free. Protodd's catalog lists a Gateway at
150 minerals; Cybernetics Core 200 minerals; Singularity Charge 150 minerals
and 150 gas; Robotics Facility 200 minerals and 200 gas; Observatory 50
minerals and 100 gas; Observer 25 minerals and 75 gas; and Nexus 400 minerals.
These costs exclude the Zealots and Dragoons in the screen. The safe line
therefore deliberately risks a later expansion to buy detection and two-gate
production; the economic line deliberately accepts less early production only
after positive enemy expansion evidence. These are hypotheses to compare, not
validated timings.

## Research notes

- Blizzard's PvT guide describes Leg-Enhanced Zealots exploiting Siege Mode's
  minimum-range gap and recommends splitting groups against spread Tanks. It
  distinguishes lightly defended field tanks from fortified Tank/Bunker/Turret
  lines, warns against using Storm as a general answer to those fortifications,
  and identifies Recall as a positional response. This supports retaining
  conservative frontal-engagement safeguards and prioritizing supported
  Dragoon/Observer/Zealot-speed transitions.
- The authored PvT strategy discussion in *How to Improve by Ver* describes a
  2-Gateway Observer line as safer against Factory-based aggression but
  economically behind a Factory-Command-Center opening. That tradeoff motivates
  two separate IDs; it does not establish the thresholds or timings used here.
- The verified local Stardust snapshot has explicit PvT strategy names and
  records strategy changes when the recognized enemy plan changes. The local
  PurpleWave snapshot includes both fixed and history/win-probability strategy
  selection. These were inspected as architecture references only. Stardust's
  tournament-use consent condition is preserved; no opponent source was copied.
- The cited StarCraft high-level strategy paper frames switching as a decision
  under partial observation. Protodd starts with deterministic matched arms;
  bounded loss-based selection is deferred until the arms have explicit,
  uncontaminated comparisons.

Research links:

- Blizzard's [Protoss-versus-Terran guide](https://classic.battle.net/scc/protoss/pvt.shtml)
  recommends speed Zealots and split approaches against field Siege Tanks,
  while treating Tank/Bunker/Turret blockades as a different and more demanding
  problem. Its Carrier and Recall suggestions are not implemented here because
  the current task prioritizes already-supported production and control.
- Blizzard's [Observer unit page](https://classic.battle.net/scc/protoss/units/observer.shtml)
  identifies Observers as the mobile cloak detector and gives their cost as 25
  minerals/75 gas and build time as 40 seconds. That supports an existing-unit
  detection checkpoint without inventing a new mine-clear mechanic.
- Gehring et al., [High-Level Strategy Selection under Partial Observability
  in StarCraft: Brood War](https://arxiv.org/abs/1811.08568), motivates treating
  opponent strategy as uncertain from partial observations. It does not imply
  that this prototype should learn online before matched arm results exist.
- Ver, [How to Improve](https://tl.net/staff/stet_tcl/How_to_Improve_by_Ver.pdf),
  is an authored strategy discussion used only for the qualitative tradeoff
  between a two-Gateway Observer safety line and a faster Factory-Command
  Center economy; no build thresholds are copied from it.

All frame, count, pressure, and memory cutoffs in this patch are local
experimental choices. No opponent source threshold was transplanted.

## Opponent inventory for a later controlled test

The original checkout's `ladder/bots` contains these read-only installed
packages and configuration facts:

| Install | Observed local configuration | Mech test value |
| --- | --- | --- |
| Steamhammer | `Steamhammer_5.3.6.json`, prepared `eval_PvT.txt`; PvT Terran strategy pool has Vultures (weight 50), Tanks (10, alternate weight 15), SiegeExpand (10), BBS (10), and VultureDrop (15). It is configured Random race, with plan recognition enabled. DLL SHA-256 `EEF137417A4D3CC9141A1B88C9C233AE48DC66A886F8E04A73D69D85D4603AC5`; config SHA-256 `FB2A5FFD61B092F6BB5708F19FD3594F315A885ABE7940E16F1899DBEB07F8E2`; prepared PvT SHA-256 `80E590B74F6E5E337026DC977272F4D888D3A849E9411B46A28D6B5CFF5BB971`. | Best already-installed mech-capable candidate. Its strategy is selected from the prepared/configured pool, so a run is a confirmed mech result only if the game log shows Factory/Tank/Mine cues. The user-reported current ZergLurker game remains Zerg. |
| UABTerran | `UAlbertaBot_Config.txt` selects `Terran_MarineRush` by default. It also defines `Terran_TankPush` (two Factories, Machine Shops, Siege Mode, then four Tanks) but the default opponent mapping does not select it. DLL SHA-256 `A99DF20B634298D746114673596A1CE4D6A152D148CD086F8FA67BC803598183`; config SHA-256 `E6F12E4AA2A1780F36A02032FF7667664E492D70517AABE5ABB65187DC4E3851`. | Possible fixed TankPush arm in a separately owned test configuration. Prior notes report UABTerran crash/runtime concerns, so first require a baseline health gate. Do not change the installed config in place. |
| Iron | Terran AIIDE 2016 DLL only; DLL SHA-256 `B5AA58AD22B54C538C9D3366B4389707F28096405019AFBFD173AD636E6716BB`. | No local strategy config or prepared-data files to identify a mech build. |
| insanitybot | Terran AIIDE 2025 DLL and source; no separate opening config present. DLL SHA-256 `3456E1A211487AA93BD8245731D2D831C396E36148091ECD186B224A254DA840`. | Installed, but this inventory does not identify a deterministic mech opener. |
| VOID | Terran 0.6.1 proxy package; README identifies a Two Rax Academy opener. JAR SHA-256 `4F2C303AE5C772A0DD3F7D968C4CED6B9CACD59EF192EA58F43EB7EAFB9F02AA`; included `VOID.dll` is zero bytes. | Not a mech opening. Proxy launch/runtime health must be checked separately. |

The checked-in `ladder/ladder5.local.json` lists Iron, insanitybot, and VOID as
Terran, Steamhammer as Random, and no fixed UABTerran entry. Earlier PvT notes
also name UAlbertaBot Terran. These labels alone do not prove a mech game.

The current Steamhammer ZergLurker test is a Zerg opponent and must remain
classified as Zerg. Do not use it as a Terran-mech test. Before gameplay,
preserve the prepared/read/write data and clone the relevant opponent config to
the isolated evaluation root. Steamhammer is the least invasive first candidate
because its installed PvT pool already includes Factory/Vulture/Tank strategies;
UABTerran TankPush is an alternate only after its crash gate passes. Match logs
must confirm the actual opponent race and observed Factory/Tank/Mine cues.

For a later comparison, hold source, HYBRIDON mode, frozen weights, map, seed,
host side, opponent binary/configuration/data, and opening-style history fixed;
change only `PvT-strategy.txt`. Record normal game completion separately from
strategic result. Review first Factory observation, second Command Center
observation, second Gateway/Core/Range/Observer timings, first Nexus timing,
mine detection coverage and losses, Tank trades, Probe count, and attack entries
into fortified lines. Do not start those games until the parent assigns CPU
capacity and separately authorizes gameplay.

## Local code references

- `include/protodd/Strategy.hpp`: stable IDs and explicit plan tag.
- `src/core/Strategy.cpp`: the two plan variants and evidence-gated transitions.
- `src/bwapi/ProtoddModule.cpp`: deterministic selector, opponent/cue logging.
- `tests/test_main.cpp`: core scenarios for the arm requirements and
  positive-evidence gate, proxy/static-contain/aggression vetoes, expiring
  Command Center memory, and missing, queued, killed, incomplete, or upgrading
  safe-arm checkpoint requirements.

The native tests and Win32 build have not been run in this task. They remain
source-level regression scenarios until a parent-assigned CPU/build window.
