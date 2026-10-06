# Trained/native hybrid controller - 1 October 2026

## Controller Ownership

The user's requested combination is implemented as an opt-in local evaluation
build. The six-slot model runs with embedded training weights while the native
bot continues its worker management, economy, build orders, expansion planning,
scouting, detection, army evaluation, combat micro, transports and maintenance.
It no longer returns before those systems run, as exclusive learned control did.

Learned attack-unit proposals enter the existing CommandBus at priority 81.
They are restricted to immediate legal shots by Zealots, Dragoons, Archons and
Dark Templar in an accepted local fight. Workers, construction, production,
Observers, transports and specialist units remain native-owned. Proposals expire
after 24 frames and get one arbitration attempt. Retreat, screen, kite, splash
spacing, recharge, cloak safety and Storm orders retain higher priority. Learned
orders use the ordinary bridge, without the exclusive controller's unit leases.

This initial integration does **not** allow learned build orders, economy
takeover, travel objectives, spells or transport orders. The trained controller
has not demonstrated competent control of those scopes. This is a combination
with explicit ownership, not two independent bots issuing competing orders.

`PROTODD_WHOLE_GAME_HYBRID` requires both whole-game control and a local evaluation
build. `PROTODD_NATIVE_ALLIN_OPENING` independently selects the native opening
repertoire in either native or hybrid DLLs. Tournament promotion remains blocked. Hybrid results are explicitly
rejected by the exclusive whole-game controller audit, so native gameplay cannot
be credited as evidence that the full learned model passed its gates.

## Build And Replay

```powershell
./scripts/build-trained-controller.ps1 -BuildDirectory build/hybrid-win32-20261001
```

The script verifies the six-slot export manifest and weights digest, embeds those
weights, builds the Win32 `ProtoddEvaluation.dll`, runs tests and prints digests.
Its all-in opening switch changes only the native opening repertoire; it does
not select the learned controller mode. Use `scripts/build-opening-comparison.ps1`
to build the complete native/hybrid by standard/all-in matrix with separate manifests.
Default weights are the opening continuation below. `-Weights` selects another
intact export; `-Exclusive` reproduces the research controller without the hybrid.
Model checkpoints and game artifacts are local ignored files, not Git payloads.

Arena comparisons use `--whole-game-hybrid-mode shadow` or `target`. Both modes
run the same model, weights and native systems; shadow mode gives proposals no
command authority. The selected mode file is hashed in the campaign manifest.
Every live game logs loaded weights, controller mode and hybrid authority.
`HYBRID_SUMMARY` records legal proposals received, attack proposals, submissions
and accepted commands. Inference heartbeats now include STOP-only predictions.

## Training Provenance

The 7,200-game full teacher checkpoint is preserved. Continuation initializes
every tensor from that trained six-slot checkpoint, not from an untrained head
or only a single-command backbone. It runs 256 exploratory updates using six
training games and six separate development games, frames through 3,600, batch
8, history 8, learning rate 0.00001 and seed 45. Those development games are not
globally untouched by all historical models. No live outcome labels or online
updates were used.

Local files:

- Full teacher: `artifacts/replay-learning/whole-game-multislot-full-2400x3-width512-20260923/teacher.pt`
- Continued teacher/export: `artifacts/replay-learning/whole-game-opening-continuation-20260930`
- Training-source reconstruction: `build/trained-banana-20260930/training-source`
- Exclusive game review: `build/trained-banana-20260930/review.json`

The source reconstruction was captured after fitting and verified against every
module digest recorded in the run. It was not frozen before launch. The later
input-bound validator is covered by tests but was not in the loaded fit process.

| Artifact | SHA-256 |
| --- | --- |
| Full teacher | `e190bcde07eeb6cbdafb65d880df1bc84c3f0e1ac49d86184c09f42486b20642` |
| Original exported weights | `1c91df745f63bebd94e5e142fecb5ffbc4218781ad24eff5502f109f9d664e87` |
| Continued teacher | `2a51b4ccdc6c88f363ffb7cd6b57a5e28029f264080672ef16c89c5025c1dd9a` |
| Continued exported weights | `5fac1e322748d78c878a7f0043e3797a7ca3ff5e3a7b7a84d1233bcb33532d16` |
| Continued run | `8ac4814f99ccec62ab9ced3dcd8fceb29dec85e7cd92dd9f2f414e7d55762746` |

Python/Win32 parity passed on all three matchups, maximum absolute error
0.00000762939453125. Opening replay audits improved complete signatures only
5 to 6 out of 1,138 scored slots. Both models scored zero positions within
64 pixels out of 753. This is not evidence of a strong learned policy.

## Exclusive Control Failures

Both original-weight games and both continued-weight games lost to stock
BananaBrain's verified `PvP_3gaterobo` on Benzene with both host sides. Each game
stayed at five Probes, one Nexus, no Pylon, no Gateway and no army. Continued
weights reached a 1,520-mineral bank in one game. All four ended normally below
the 12,000-frame development cap, without crashes or timeouts. The candidate
also restored telemetry, so this was not a strictly weights-only comparison.

These failures motivated the hybrid's native opening ownership. Neither
exclusive model is promoted; a few extra replay matches did not fix gameplay.

## Hybrid Evaluation

Frozen campaigns: `build/hybrid-banana-20261001/{shadow,target}`. Same Win32 DLL,
continued weights, fixed stock BananaBrain `PvP_3gaterobo`, Benzene, both host
sides, frozen learning, full 86,400-frame ceiling, development slow-frame
allowance 640. Actual maps, seeds and host sides must match before comparison.

Evaluated DLL: `05077e6b9d521f7cc4193babc0b2a99e59c96d1a4a2b1b5ec8e5dcb21731481f`.
Compiled source fingerprint: `b6f1b0ab7ec265ddb6501b81dd1b73f12186e7b3f1bbd52017adf54b2ccc557f`.
Source snapshot and build record are retained at the campaign root.

Review with:

```powershell
./build/model-venv/Scripts/python.exe tools/hybrid_campaign_review.py `
    build/hybrid-banana-20261001 --output build/hybrid-banana-20261001/review.json
```

Both arms finished 0/2 wins with normal paired reports, no crashes and no
timeouts. Actual maps, seeds and host sides matched. No legal attack proposal was
generated or accepted in any game; this is not evidence of learned improvement.

| Mode | Loss Frames | Peak Probes | Peak Gateways | Peak Dragoons | Max Callback (ms) |
| --- | --- | --- | --- | --- | --- |
| Shadow | 20,927 / 24,399 | 34 / 32 | 7 / 4 | 18 / 14 | 49.082 / 115.640 |
| Target | 24,120 / 24,492 | 51 / 50 | 8 / 7 | 18 / 18 | 119.948 / 49.053 |

Target games reached three Nexuses instead of the exclusive model's one-base
starvation, but still lost. The strict 42 ms callback gate failed in both arms.
Development timing/load differences prevent attributing economy differences to
the model, which issued zero orders. Local replay-mining work began near the end
of this pilot; it is not an isolated performance benchmark. Every owned arena
process was stopped after the games. Review receipt:
`build/hybrid-banana-20261001/review.json`.

The user's subsequent request is to mine ladder data for multiple real all-in
builds with explicit transitions. The hybrid alone does not fix passive strategic
play. No general playing-strength claim or tournament promotion follows here.
