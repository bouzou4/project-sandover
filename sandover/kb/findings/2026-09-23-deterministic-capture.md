---
title: Can the rig capture a 16-shot set unattended, with a repeat run giving FLIP ≈ 0 per shot?
date: 2026-09-23
issue: #10
status: verified
sha: 5b7218d
---
## Answer (≤5 lines)
Yes, with capture mode on. `sandover-capture run shots/auto-v0.yaml` takes 16 shots
(4 continue points × dawn/noon/dusk/night) in about 60 s on 4 concurrent instances. Two
runs: mean FLIP 0.00033 over the set, worst shot 0.0018, 11 of 16 shots ≤ 0.00006. What is
left is small moving creatures (sunken fish, a jungle critter): per-pixel max ≈ 0.99 on a
few hundred pixels. Particles, effects and wind are off by default; turned on they differ.

## Evidence (path:line@sha / URL, short excerpts)

### Why game time followed the wall clock
`display-frame-start` sets the frame's time-ratio from a timer that reads the host clock
(`goal_src/jak1/engine/ps2/timer.gc:25@5b7218d`, `(- (get-bus-clock/256) *timer-reset-value*)`).
Frames under 1.3 frame-times count as 1.0 (`goal_src/jak1/engine/draw/drawable.gc:989@5b7218d`),
longer ones as 2, 3 or 4, and `base-frame-counter` advances by `ratio × 5` ticks. Loads and
hitches therefore add a variable number of game-time steps. Capture mode adds
`*fixed-frame-step*`, which forces 1.0.

### What else made two runs differ, and the fix (all verified by A/B runs)
| Source | Where | Fix in the rig |
|---|---|---|
| Frames spent loading vary (e.g. 610 vs 615 ticks for jungle-start) | async DGO loads | after `RIG:READY`: clocks moved to a per-shot epoch, `reset-actors 'life`, particles killed, births paused 2 frames |
| Follow camera depends on history, and actor births use the camera | `entity.gc` birth distance | camera fixed to the continue point *before* the actor reset |
| `rand-vu` stream | global, used by launchers, fish, wind | reseeded after the reset |
| Wind spring state is renderer-side and integrates every drawn frame | `Tie3.cpp:857@5b7218d` | `*pc-wind-reset*` (new) zeroes it; the 64 GOAL wind forcing slots are zeroed too (`wind.gc:15`) |
| Cloud scroll accumulates per frame | `sky-tng.gc:162@5b7218d` | offsets zeroed |
| Particle clock | `sparticle.gc:451@5b7218d` `*particle-300hz-timer*` | zeroed |
| Actor pause distance seeks toward its limit each frame, so it is still moving after a load | `entity.gc:1026@5b7218d` | set to `birth-dist` |
| Water ripple phase accumulates since level load | `ripple.gc:47@5b7218d` | wave offsets zeroed after actors are reborn |
| Mood effect weights (e.g. sunken caustics driven by a per-frame counter, `mood.gc:1012@5b7218d`) freeze at their last value when effects are turned off | `mood.gc` | with effects off, all eight palette weights are zeroed; the hour table rewrites the sun slots |
| Part spawners launch a different number of particles right after the reset, and every launch draws random numbers | `sparticle-launcher.gc:685@5b7218d` | with particles off, `spawn` returns early (new), so launchers draw nothing |

Things that did not help: reseeding `rand-vu` every frame (made sunken worse, 0.02).

### What remains
The `rand-vu` state at the capture frame still differs between runs (printed in
`RIG:STAGED`). Instrumenting `rand-vu` showed per-frame call counts that differ from the
first frames after the reset, from callers such as `eco-blue` pickups and the main loop.
Their count follows state the reset does not touch (sound and similar async inputs are the
likely drivers; not proven). Visible effect: sunken fish outside the window and one small
creature in jungle-start end up in different places.

### FLIP, 16-shot auto set (2560x1440, MSAA 4)
Run A and run B: 4 instances each, same command. N1: 1 instance (other shot order per
instance, so it also checks that history does not leak between shots).

| shot | A vs B mean | A vs B max | A vs B px > 0.1 | A vs N1 mean | A vs N1 max |
|---|---|---|---|---|---|
| village1-hut-dawn | 0.000000 | 0.000 | 0.000% | 0.000050 | 0.278 |
| village1-hut-noon | 0.000000 | 0.016 | 0.000% | 0.000000 | 0.016 |
| village1-hut-dusk | 0.000000 | 0.000 | 0.000% | 0.000000 | 0.000 |
| village1-hut-night | 0.000000 | 0.000 | 0.000% | 0.000000 | 0.000 |
| beach-start-dawn | 0.000057 | 0.164 | 0.000% | 0.000357 | 0.317 |
| beach-start-noon | 0.000000 | 0.003 | 0.000% | 0.000000 | 0.000 |
| beach-start-dusk | 0.000000 | 0.004 | 0.000% | 0.000000 | 0.004 |
| beach-start-night | 0.000000 | 0.000 | 0.000% | 0.000000 | 0.005 |
| jungle-start-dawn | 0.000450 | 0.968 | 0.130% | 0.000579 | 0.967 |
| jungle-start-noon | 0.000849 | 0.988 | 0.163% | 0.000498 | 0.984 |
| jungle-start-dusk | 0.000297 | 0.768 | 0.086% | 0.000410 | 0.767 |
| jungle-start-night | 0.000123 | 0.379 | 0.037% | 0.000527 | 0.740 |
| sunken-start-dawn | 0.001843 | 0.955 | 0.339% | 0.000814 | 0.956 |
| sunken-start-noon | 0.000000 | 0.003 | 0.000% | 0.000549 | 0.878 |
| sunken-start-dusk | 0.000673 | 0.891 | 0.134% | 0.000837 | 0.673 |
| sunken-start-night | 0.000955 | 0.832 | 0.180% | 0.000955 | 0.832 |
| **set** | **0.000328** (worst 0.00184) | **0.988** | | **0.000348** (worst 0.00095) | **0.984** |

Progression on village1/beach/jungle/sunken noon, same pair method: no fixes 0.0089
(village1+beach only) → fixed step, fixed camera, reset actors 0.0078 → + wind reset, ripples
0.0048 → + particles off 0.0003 → + launcher skip, effect weights 0.0003 with sunken ≈ 0.

With particles on, village1 chimney smoke and beach eco vents differ (0.001-0.005 per shot).

### Timings (16 shots, `out/` current)
| Instances | Wall time | (mi) phase | shots phase |
|---|---|---|---|
| 1 | 71 s | ~11 s | ~51 s |
| 4 | 57-65 s | ~35-40 s | ~15 s |
| 8 | 106 s | ~90 s | ~13 s |

CPU mean/max: 17/31% at 1, 29/71% at 4, 41/100% at 8. GPU memory at most 7.8 GB at 8.
The per-instance `(mi)` (needed for the compiler's type info) is the bottleneck when
instances start together. Earlier in the session one run waited about 12 minutes for
another holder of the game lock; the numbers above are from runs without contention.

### Concurrency facts found on the way
- `gk --port` was declared with `add_flag`, so it swallowed no value (`game/main.cpp:113@5b7218d`),
  and the DECI2 server ignored the port anyway (`game/runtime.cpp:114@5b7218d`). Both fixed.
- A fresh goalc's `(mi)` always recompiles every file (`goalc/make/Tools.cpp:25@5b7218d`:
  a file unknown to this compiler instance needs a run) and rewrites every `.o`, then every
  CGO/DGO (`Tools.cpp:37`, `options.write = true`). New `OPENGOAL_MAKE_NO_WRITE` keeps the
  compile and skips all writes; verified by `KERNEL.CGO` mtime staying put over a 4-instance run.
- goalc and gk rotate logs in `$SANDOVER_ROOT/log/` at start-up; simultaneous starts race
  ("Failed to setup logging: The system cannot find the file specified").

## Implications for the design
- Golden-image comparisons can use this rig: defaults (effects, wind, particles off, fixed
  camera) give FLIP ≈ 0 on static scenes and ≤ 0.002 per shot with small critters.
- Lighting A/B should compare against a golden from the same manifest order; a shot's clock
  depends on its manifest index.
- Anything that needs particles or effects in the frame needs its own randomness control
  (per-consumer RNG), not a global reseed.

## Follow-ups
- Replace the per-instance `(mi)` with a cheaper type load; it is 60-85% of wall time at 4-8
  instances.
- Track down the remaining `rand-vu` consumers (eco, sound-timed) if critters matter.
- Wind on: the reset makes the renderer side deterministic; not yet measured with wind on.
