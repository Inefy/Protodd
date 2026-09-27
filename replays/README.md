# Human replay source archive

`cwal-source/` preserves the locally downloaded [cwal.gg replay corpus](https://cwal.gg/replays)
by matchup. Each ZIP contains the original `.rep` bytes and paths. Git LFS stores
the six archives; clone with Git LFS available, then run `git lfs pull` if the
ZIPs are pointers after checkout.

This snapshot contains 59,391 source replay files in six archives (5.075 GB).
The frozen release marks 11,183 as usable training games, 1,380 validation,
6,116 test, and 40,712 unassigned. Its source manifest SHA-256 is
`646818d53e45b1a1c20869c011afe3d705f50be003550a410d45db6561f6371a`.

The frozen Protoss data-release manifest determines split labels. The bundle
manifest lists a SHA-256 for every replay, a SHA-256 for every ZIP, and the
SHA-256 of the release manifest used to assign splits. The bundle includes
source replays outside the qualified Protoss release as `unassigned`. Those
files are preserved as source material; they are not approved training data.
The frozen validation and test replays are also preserved in the source ZIPs,
but are excluded by the default restore command.

Restore only the approved training split into the path expected by the
existing training tools:

```powershell
python -m training.replay_bundle restore --bundle replays/cwal-source --output artifacts/cwal-dataset
```

To recover the entire source corpus for a new, separately reviewed dataset
release, add `--scope source`. Do not use that option as a shortcut for model
training: it includes validation, test, and unassigned replays.

Recreate the archives from the original local downloads and frozen release:

```powershell
python -m training.replay_bundle create --manifest artifacts/replay-learning/protoss-data-release-20260922/manifest.json --source artifacts/cwal-dataset --output replays/cwal-source
```

The archive stores replay files only. Arena match replays, bot logs, local maps,
third-party executables, and extracted features are separate artifacts.
These are third-party game recordings. The repository's code license does not
grant rights to the replay content.
