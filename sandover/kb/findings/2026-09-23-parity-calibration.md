---
title: Where is the parity gate's FLIP threshold, and does the 56-shot auto-v1 set hold it?
date: 2026-09-23
issue: none (phase 0 parity gate)
status: verified
sha: a664140c3
---
## Answer (≤5 lines)
On `auto-v1` (56 shots, 2560x1440, MSAA 4): F0 = 0.00359 (p99 of per-shot FLIP between two
cold runs), m = 0.00806, gate threshold F0 + m = 0.01165. The smallest known-small change,
a one-level gamma nudge, gives FLIP ≥ 0.0197 on every shot, so it fails the gate while the
noise floor passes with 3x headroom. `auto-v0`: F0 0.00133, m 0.00987, threshold 0.01120.
auto-v1 captures in 142-149 s on 4 instances with `out/` current.

## Evidence (path:line@sha / URL, short excerpts)
- Tool: `sandover/tools/sandover/parity/` (`sandover-parity calibrate|golden|check|determinism|watch`),
  runbook `kb/howto/parity.md`. Build: staged Release copy of master at engine sha a664140c3,
  pinned under `$SANDOVER_DATA/parity/bin/`; every run below used that one copy.
  Caveat: that sha was inferred from the checkout at pin time, and the copy's `gk` is dated
  04:47 local, before the engine commits 7bbeb0042 and a664140c3 landed. Re-pin with a
  `build.json`-recorded sha before these goldens gate anything.
- Calibration entries: `$SANDOVER_DATA/parity/calibration.json`, keys
  `auto-v1|2560x1440|msaa4|a664140c3` and `auto-v0|2560x1440|msaa4|a664140c3`; runs, floor,
  perturbation tables and noise error maps under `$SANDOVER_DATA/parity/calib/<key>/`.

Noise floor, two cold runs, same build:

| Set | Shots | F0 (p99) | Mean of means | Median | Worst shot | Pixel-exact shots |
|---|---|---|---|---|---|---|
| auto-v1 | 56 | 0.00359 | 0.00036 | 2e-7 | snow-start-noon 0.00374 | 24 |
| auto-v0 | 16 | 0.00133 | 0.00037 | 5e-5 | sunken-start-dawn 0.00137 | 3 |

auto-v1 worst shots: snow-start noon/dawn/dusk 0.0031-0.0037 (about 22k pixels differ),
sunken-start night 0.0022, sunken dawn/dusk 0.0011. 37 of 56 shots at or below 0.00006.

Perturbations applied in post to run 0, FLIP mean against the unperturbed image (auto-v1):

| Perturbation | Min | Median | Max |
|---|---|---|---|
| one-pixel shift, x | 0.0211 | 0.0383 | 0.1155 |
| one-pixel shift, y | 0.0309 | 0.0509 | 0.1423 |
| gamma, mid-grey moves one 8-bit level | 0.0197 | 0.0360 | 0.0424 |
| +1 level on every channel | 0.0332 | 0.0375 | 0.0423 |

m is set so the threshold sits halfway between F0 and the smallest perturbation minimum
(0.0197): m = (0.0197 - 0.00359) / 2. On auto-v0 the smallest is the x shift (0.0211).

Gate runs with the threshold:
- `check --set auto-v1` with the same build captured fresh: PASS 56/56, worst 0.00322
  (sunken-start-noon), mean 0.00025.
- `check --set auto-v0 --captures` on the golden with every other shot gamma-nudged: FAIL,
  exactly the 8 nudged shots.
- `watch --once` (auto-v0) picked the newest staged build, the SSAO engine build with its
  feature default-off, and passed 16/16 against the a664140c3 golden.
- `determinism --set auto-v0`: mean 0.00055, worst 0.00277 (sunken-start-dawn), 5 of 16
  pixel-exact, all 16 cameras bit-equal. The rig note's 0.00033 is the same order; this
  pair's worst is above auto-v0's two-run F0, below the threshold.

auto-v1 capture time, 4 instances, 56 shots:

| Run | Wall time | Note |
|---|---|---|
| trial | 142 s | `out/` current; `(mi)` phase about 25 s |
| calibration run 0 | 149 s | `out/` current |
| golden | 190 s | slot 0 rewrote `out/` first |
| calibration run 1 | 260 s | one gk died loading sunken; 12 shots retried on one instance |
| check (fresh capture) | 339 s | during another engine build: 3 of 4 goalc missed the 60 s nREPL start; 44 shots retried on one instance |

Goldens: `$SANDOVER_DATA/goldens/auto-v1/a664140c3/` (56 shots) and
`$SANDOVER_DATA/goldens/auto-v0/a664140c3/` (16), each with sidecars and `manifest.json`.

## Implications for the design
- The calibrated parity band B1 is now a number: 0.0116 on auto-v1. FLIP LDR registers a one-level global
  change at ≈ 0.02-0.04, so "within one 8-bit level everywhere" is the practical resolution
  of the gate; smaller local changes (a few hundred pixels) can pass.
- Snow and sunken dominate the floor (moving creatures, `rand-vu` drift, per
  `deterministic-capture`). Two runs underestimate a floor: the auto-v0 determinism
  pair's worst shot (0.0028) exceeds auto-v0's two-run F0 (0.0013). The three-run rule
  and per-shot thresholds would tighten the gate on the 24 pixel-exact shots.
- The rig's retry path absorbs boot failures under CPU load, at the cost of running the
  retried shots on one instance.

## Follow-ups
- Calibrate over 3+ runs and add per-shot thresholds (`T_shot`) for pixel-exact shots.
- Raise or make load-aware the 60 s goalc start timeout; retry on several slots, not one.
- `watch` posts no commit status or PR comment yet; builds stay manual.
- Recalibrate after the SSAO build lands if the rig or out/ changes.
