# Operating metrics and denominators

The decision report exposes operating measures as diagnostics. Read every rate
with its numerator, denominator, sample coverage, and notes. These measures do
not assign a single score, predict win rate, or imply that a lower number is
always better.

## Production and supply

- **Affordable idle producer exposure** integrates idle production buildings
  over observed frames. The denominator is completed, powered Nexus, Gateway,
  Stargate, and Robotics Facility unit-frames that are ready, empty, unlocked,
  and past the command latency interval. The numerator is the subset where at
  least one supported unit can be made using unreserved minerals, gas, supply,
  and the live BWAPI `canTrain` / `canMake` checks. Values are estimates between
  observations; higher values can mean poor spending, but also reflect limited
  demand or intentionally preserved reserves.
- **Unintended supply-cap exposure** divides unintended hard-cap frames by all
  frames with a finite supply cap. A cap during the early opening while a Pylon
  is already building is separated as a deliberate opening pause. Tight supply
  (fewer than four free supply) remains a different gauge from a hard block.

## Detection, workers, and contact

- **Useful detection arrival** begins when a logged squad needs detection and
  no detector is ready. It counts as useful only if the same squad still needs
  detection, detection becomes ready, units remain, and enemies are still in
  contact. The denominator is resolved waits. A wait still open at the last
  sample is censored and shown separately; no EOF is treated as arrival.
- **Worker deaths** shows exact own worker-loss callbacks, with a rate per 1,000
  observed Probe unit-minutes. Probe exposure integrates the previous HEALTH
  count only across gaps of at most 48 frames. Long gaps are excluded; this
  denominator does not reward abandoning workers or avoiding fights by itself.
- **Army contact exposure** divides squad unit-frames with a locally observed
  enemy in the preceding sample by all integrated squad unit-frames. Only gaps
  up to 48 frames contribute; squads absent from a sample are not extrapolated.
  The rate is descriptive: low contact can mean safe maneuvering or failed
  engagement, and high contact can mean useful pressure or dangerous exposure.

## Reservations, commands, and recovery

- **Reservations by reason** counts sampled `MACRO` rows per reason and reports
  the share marked reserved. Heartbeats and state changes are samples, not
  unique reservation events or continuous reservation time. Cost figures are
  mean recorded action costs on reserved samples; they are not totals spent.
- **Command failures** divides BWAPI-issued commands not accepted by all issued
  commands. Actions rejected before issue are listed separately, outside that
  denominator.
- **Spell command acceptance and effects** reports BWAPI acceptance by command
  type and technology separately from later effect evidence. Accepted Storm,
  Stasis, Recall, Feedback, and Archon-merge commands are followed for bounded
  observation windows. The trace looks for a matching storm bullet and affected
  enemy, a newly disabled target with caster energy spent, recalled units near
  the Arbiter's cast location, a Feedback target with energy and durability
  decreases, or an Archon near the merging Templars. An observed change is
  correlated by time, actor, target/location, and available snapshots; it does
  not prove the command caused that change. A complete window with no matching
  observation is counted as ineffective; missing actors, lost visibility,
  incomplete baselines, unsupported spell types, and interrupted windows are
  censored. The report gives the ineffective share only over observed plus
  ineffective outcomes, alongside censored and accepted-but-unattributed
  counts. Legacy traces without effect counters keep the rate unknown. Rejected
  commands are not casts.
- **Incident recovery** counts episodes that have an explicit `active=0` record
  as resolved and divides by observed episodes. Episodes still active at EOF
  are censored, reported separately, and never counted as recovered. Observed
  duration is not extrapolated across missing heartbeats.

Use these signals alongside combat outcomes, resource spending, scouting, and
the retained event trace. No metric here rewards avoiding every fight,
abandoning scouts, or canceling all spending, and no single ratio is an
optimization target.
