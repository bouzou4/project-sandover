# Parity gate how-to

`sandover-parity` answers one question: with every toggle off, does a build still render
the shot set like vanilla? It captures with the rig (`rig-capture.md`), compares against a
golden generation with FLIP, and passes a shot when its FLIP mean is at most `F0 + m`, the
calibrated band B1. Numbers: `kb/findings/2026-09-23-parity-calibration.md`.

Current goldens: engine sha `f953354f0` (`goldens/auto-v0/f953354f0/`,
`goldens/auto-v1/f953354f0/`), pinned build `f953354f0-20260923-164511-6ecf9f700b`. Refresh
details and thresholds: `kb/findings/2026-09-23-golden-refresh.md`. The earlier
`a664140c3` goldens were captured from a binary older than their label and are retired
(not deleted) under `$SANDOVER_DATA/parity/retired/`.

## Commands

```sh
cd sandover/tools
uv run sandover-parity check --build current            # auto-v1 vs its newest golden
uv run sandover-parity check --set auto-v0 --build current   # quick set
uv run sandover-parity check --build 20260923-044755-8ccfebec6e   # a staged copy by name
uv run sandover-parity calibrate --set auto-v1          # two runs + perturbations -> F0, m
uv run sandover-parity golden --set auto-v1 --reason "upstream merge <sha>"
uv run sandover-parity determinism --set auto-v0        # two runs, FLIP per shot
uv run sandover-parity watch                            # local CI poller (below)
```

`--build` is `current` (stage the current `out/build/Release/bin` like the rig does), a
directory holding `gk`/`goalc`, or the name of a copy under `$SANDOVER_DATA/parity/bin` or
`$SANDOVER_DATA/bin`. `--parallel N` sets game instances (default 4). Every capture runs on
instance slots, so another rebuild or game never collides with it.

From a worktree, run with `uv run --project <worktree>/sandover/tools` and set
`SANDOVER_ROOT` to the main checkout (game data, `out/` and `goal_src` live there) and
`SANDOVER_DATA` as usual.

## check

1. Picks the newest golden generation of the set (`--golden <sha>` for another) and the
   calibration entry for the golden's set, resolution, MSAA and engine sha (falling back to
   the newest entry with the same set, resolution and MSAA).
2. Captures the set with the candidate build into
   `$SANDOVER_DATA/parity/runs/<stamp>-check-<set>/captures/` (`--captures DIR` skips this
   and gates an existing capture).
3. Per shot: sidecar check (continue, hour, resolution, MSAA, staging flags equal; camera
   within 1e-3), a mismatch is status `harness`; SSIM/PSNR pre-check (size, black frame,
   channel swap, SSIM < 0.5) is status `precheck`; FLIP LDR mean vs the threshold is `pass`
   or `fail`. A missing PNG is `missing`.
4. Writes `report.json` (every number, per shot), `summary.md` (table) and `maps/<shot>.flip.png`
   (FLIP error map, magma) for every shot. Exit code 1 if any shot is not `pass`.

## calibrate

Captures the set twice with one pinned build (`--runs A B` reuses two existing capture
dirs). `F0` is the p99 of the per-shot FLIP means between the runs (band B0). Then four
known-small perturbations are applied in post to run 0 and FLIP'd against it: a one-pixel
shift in x and in y, a gamma change that moves mid-grey by one 8-bit level, and +1 level on
every channel. `m` puts the threshold halfway between `F0` and the smallest FLIP any of
those gives on any shot, so each of them fails the gate and the noise floor passes. If a
perturbation lands inside the floor, `m` is 0 and a warning is printed.

Output: an entry in `$SANDOVER_DATA/parity/calibration.json` keyed
`<set>|<w>x<h>|msaa<n>|<engine sha>`; runs, `floor.json`, `perturbations.json` and every
noise error map under `$SANDOVER_DATA/parity/calib/<key>/`; the per-shot floor in
`$SANDOVER_DATA/goldens/<set>/noise.json`. Recalibrate whenever the shot set,
resolution, MSAA, rig or graphics driver changes.

## golden

Pins the build (copies it to `$SANDOVER_DATA/parity/bin/<engine sha>-<staged name>/` with a
`pin.json`; the rig's pruning never looks there), captures the set into
`$SANDOVER_DATA/goldens/<set>/<engine sha>/` and publishes it only if every shot succeeded.
`manifest.json` records set, manifest digest, engine sha, binaries, date, `--reason`,
resolution, MSAA, capture time and a sha256 per PNG. Sidecars gain `schema`, `set`,
`manifest_digest`, `build`, `engine_sha`, `bin` and `goal_src`.

Staging writes `build.json` into each copy: the checkout's last commit touching anything
outside `sandover/`, recorded only if no engine source (`common`, `decompiler`, `game`,
`goalc`, `third-party`, `CMakeLists.txt`) is modified or newer than `gk`; otherwise
`engine_sha` is null with the reason. `golden` and `calibrate` refuse a build whose sha is
unknown (including copies staged before `build.json` existed): rebuild from a clean
checkout and re-stage, or pass `--engine-sha`, which must not contradict a recorded sha.
`check` records the candidate's sha, or `unknown` with a warning.

GOAL code is compiled from the main checkout's `goal_src` at capture time, not from the
pinned copy. Sidecars, `manifest.json`, calibration entries and `report.json` record
`goal_src`: the checkout sha and whether `goal_src` had uncommitted changes; a dirty
`goal_src` or a checkout that moves during a capture prints a warning.

Refresh policy: a new generation per upstream merge (after rebuilding
vanilla), after a rig change once determinism is re-proved, and on any resolution, MSAA or
shot-set change; after a driver update re-run `calibrate` and refresh only if the floor
moved. A refresh is its own PR with the reason stated; a golden is never refreshed to make
a failing gate pass. `--replace` moves an existing generation of the same sha aside instead
of overwriting it. The tool keeps every generation and prints a reminder when there are
more than two; older ones are deleted only with explicit approval.

## determinism

Two (`--runs N`) cold runs of one build; per pair and shot: FLIP mean and max, differing
pixel count, max abs difference, camera bit equality. Report under
`$SANDOVER_DATA/parity/runs/<stamp>-determinism-<set>/`.

## watch (local CI poller)

`sandover-parity watch [--set auto-v0] [--interval 300] [--once]` fetches `origin/master`
every interval; on a new commit it runs `check` with the newest staged build
(`$SANDOVER_DATA/bin`, newest by name) into `$SANDOVER_DATA/ci/<stamp>-<sha>/` and records
the result in `$SANDOVER_DATA/parity/watch.json`. It never builds: builds are manual for
now, so the build it checks is whatever was last staged (any rig run stages the current
build). No commit statuses or PR comments yet.

## Shot sets

| Set | Shots | Use |
|---|---|---|
| `auto-v0` | 4 continue points x dawn/noon/dusk/night = 16 | quick set: engine PRs, determinism |
| `auto-v1` | 14 continue points x 4 hours = 56 | full parity and goldens |

`auto-v1` adds village2, a two-level overlap (`firecanyon-end`: village2 and Fire Canyon
both displayed), rolling hills, swamp, misty, village3, snow, the indoor fixed-slot Dark
Cave (`darkcave-start`), and the fixed-slot quick levels Lava Tube and Citadel.
