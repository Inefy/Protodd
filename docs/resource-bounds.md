# Runtime resource bounds

Tournament builds run as a 32-bit BWAPI process, so persisted inputs and retained match state need explicit ceilings. The bounds below describe the current implementation; they do not imply that every combination has been exercised in a live game.

| Resource | Bound | Safe behavior |
| --- | ---: | --- |
| Protodd text/control files | 64 KiB per file | Oversized reads are rejected; optional controls use an empty or disabled mode. |
| Opponent history file | 4 MiB | Invalid or oversized history suppresses the write so the existing file is preserved. |
| Opponent history rows | 4,096 records and 1,024 outcome IDs per record | Excess rows are ignored; duplicate outcomes remain idempotent. |
| Opponent and map fields | 128 UTF-8 bytes each; opponent filename 120 bytes | Oversized values are rejected before persistence. |
| Policy table snapshot | 64 MiB input | Invalid input disables that optional policy. |
| Learned macro model | 8,000,000 float parameters, 32 MiB weights, width at most 2,048 | Dimensions and the full parameter count are checked before model state changes. |
| Whole-game model | 16,000,000 float parameters, 64 MiB tensor values, width at most 2,048 | Tensor sizes are checked before allocation; path input is streamed, embedded spans are read without a full-file copy, and trailing bytes are rejected. |
| Whole-game observation memory | 4,096 retained entity tokens | Oldest stale non-own entities are evicted with their engine-ID/published references; if current entities alone exceed the ceiling, the optional runtime shuts down and native control continues. |
| Navigation search workspace | 20 MiB per calling thread | Oversized workspaces are rejected before growth. |
| Navigation route cache | 128 KiB | Cache entries are bounded and revision checked. |
| Worker economic routing | 8 path requests per assignment pass | Clear lines are proved directly. Unproved mining/gas/transfer routes are deferred, mineral retries rotate, and deferred checks are logged. Each admitted search retains the existing 12,000-expansion limit; evacuation and local defense have separate work. |
| Defensive arrival routing | 8 searches, 1,024 expansions each per squad formation | Clear routes remain exact; bounded partial estimates rank behind proved routes. |
| Harassment routing | 8 searches, 2,048 expansions each across squad formation | Defended destinations are rejected first. Candidates are ranked before route proof; incomplete routes cannot authorize a raid. Existing missions use the conservative withdrawal path if safety cannot be established. |
| Detailed engagement admission | 32 combined units and 192 friendly/enemy pairs | Larger fights use the existing static estimate with reduced confidence. |
| Engagement geometry | 16 route searches of 512 expansions and 1,024 clearance samples, shared by both armies | Legal in-range shots need no approach proof. Incomplete route/geometry work discards the entire partial simulation and uses the reduced-confidence static estimate. |

Unit-keyed bridge and diagnostic state is cleared on game start and removed when units are destroyed, transferred, absent, or no longer observed where applicable. The optional whole-game observation/controller also caps retained entity memory at 4,096 tokens and removes matching engine-ID and publication entries when old stale records are evicted. Per-frame telemetry is written to the log stream; the module retains counters and one latest-value entry per diagnostic key instead of buffering an entire match in memory.

The stock-engine load audit now enforces a process-level budget of **256 MiB private usage** and **512 MiB peak working set**, including BWAPI, StarCraft-side data, the wrapper, and the bot. Missing or failed memory samples fail the audit. This leaves substantial room below a 32-bit process ceiling while remaining well above current measured use. The boundary test covers exact-limit pass, over-limit failure, and missing/failed samples.

Existing T118 stock-engine evidence is below the budget in both available stress cases. The 26,972-frame fog-of-war match peaked at 35,221,504 bytes private usage and 49,815,552 bytes sampled working set; the process-lifetime `PeakWorkingSetSize` field reached 52,183,040 bytes. The separate 400-supply fixture peaked at 30,220,288 bytes private usage and 45,371,392 bytes sampled and process-lifetime peak working set. Retrospective budget assessment found no failed memory samples. These runs did not enable model inference, and neither exercises malformed optional files during a full match. See `artifacts/goal-20261005/t118-combined-fog-callbacks-20261007/README.md` and `artifacts/goal-20261005/t118-t052-full-callbacks-20261007/README.md` for the raw evidence. T010 remains open until longer-duration and malformed-input behavior are exercised against the stock runtime.
