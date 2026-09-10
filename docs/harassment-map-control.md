# Dedicated harassment and map-control economy

Stable two-base economies with at least 28 workers and eight Gateway fighters
now request a separate Shuttle/Reaver package after seven minutes. The Reaver
quota includes the army reserve (two in PvP, one otherwise) plus one drop Reaver.
The robotics chain and cargo receive explicit spending priorities below a safe
expansion commitment. An existing Stargate can support four Corsairs against
observed Overlords; an existing Archives can supply two Dark Templar when an
exposed economic target is available. Emergency reinforcement cancels these
additional goals without retaining their structure reservations.

Transport missions claim their Reaver before squad formation. This prevents the
main army from redirecting cargo during rendezvous or after a drop. Economic
missions wait for two Scarabs, cross ground defenses by air, and use a bounded
two-leg bypass when observed anti-air blocks a direct approach. Both legs must
avoid known threats. The destination can be observed workers or a remembered,
occupied mineral line to investigate; remembered ownership does not assert that
workers are still present. Ground raids require traversable bypass legs, while
Shuttles can also investigate island economies.

Drops unload near a walkable mineral-line position, prioritize nearby economic
targets, and extract after a bounded firing window, ammunition depletion,
damage, defenders arriving, or a defense recall. Cargo is excluded from ordinary
squads until the mission ends. Ready attack frames are preserved. Returned
transports have a relaunch cooldown, and rendezvous and outbound travel have
timeouts. Raid orders bound pursuit and use movement orders during travel;
explicit withdrawal also overrides cloaked units' normal advance behavior.
Ranged reload movement considers the actual nearby pursuer, even when the
selected firing target is a worker or building.

After twelve minutes, an army lead against at least three observed defenses at
enemy bases can enable growth beyond matchup opening limits, up to eight total
Nexuses. This requires two existing bases, 32 workers, at least twelve mobile
fighters, and a power margin that increases with uncertainty. Safe, neutral,
non-island sites must have resources and no nearby known enemy defenders.
Growth commits one Nexus at a time, covers construction, and can use a mineral
bank before old bases are fully saturated. Contact at any owned economy cancels
this override. This is an alternative use of the lead to a frontal attack into
static defenses; it does not change the combat evaluator's acceptance threshold.

Validation covers production quotas, reservation cancellation, ground and air
bypasses, remembered economic targets, cargo ownership, worker targeting,
ammunition extraction, return cooldown, pursuit control, growth past five bases,
pending construction, and remote-base pressure. The CMake Release build also
compiles the BWAPI adapter and runs the catalog and report tests. These checks
validate decisions and compilation; match results are still needed to assess
harassment frequency, worker damage, drop survival, and win-rate impact.
