# Experimental three-race learning work

The user stopped the test campaign on 19 September 2026. Scheduled follow-up is paused. No games should restart without a new request.

This change adds basic Terran and Zerg controllers, shared tabular policy learning, reviewed-outcome training tools, frozen evaluation support, diagnostics, and regression tests. It also includes Protoss detection and conservative combat-estimation fixes, plus proxy-launcher validation.

These are experimental bots, not validated competitive improvements. The Terran policy pilot scored 0/2 for each of the empty, Q-trained and episode-return policies. The detector comparison scored 0/4 for both control and candidate. Opponent openings varied despite matched packages and requested seeds. Zerg production became functional in both immediate counting-fix tests, but both games were losses.

Earlier Terran supply planning is the latest unvalidated change. Its game campaign was stopped at the user's request; partial games must not be counted as results. All ten Release CTest suites passed before that campaign began. Local original logs, replays, frozen binaries and partial-run archives remain under the ignored build/strength-20260918 directory. Game assets, third-party binaries and machine-specific tournament files are not included in Git.

Do not train from frozen evaluation traces, count unpaired reports, or treat opponent crashes/runtime-limit outcomes as strategic wins. Thirty historical proxy wins remain suspect and are not strength evidence.
