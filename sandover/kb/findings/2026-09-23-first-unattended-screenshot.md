---
title: Can a script boot Jak 1, stage village1-hut at noon and take an engine screenshot unattended?
date: 2026-09-23
status: verified
sha: 64d72d1
---
## Answer (≤5 lines)
Yes. `sandover/tools/sandover/rig/capture.py` runs goalc and gk, drives goalc over the nREPL, and a
GOAL helper file (`rig.gc`) starts at `village1-hut`, sets hour 12, freezes time, hides the
HUD, pauses and calls `pc-screen-shot`. The PNG lands under `--config-path`. Runs take 35-45 s
and need no input. Two cold runs are **not** pixel-identical (88.9% of pixels differ, 17.5%
by more than 8/255, mean abs diff 3.7/255); determinism is follow-up work.

## Evidence (path:line@sha / URL, short excerpts)
Static claims were already swept in `sandover/kb/facts/rig.md` (at `76e8eec`; the engine
files cited below are unchanged at `64d72d1`). This note adds runtime checks.

The original specification's "Rig / automation" section, claim by claim:

| Claim | Result | Source / runtime evidence |
|---|---|---|
| `pc-screen-shot`, `pc-register-screen-shot-settings` GOAL-callable | verified (runtime) | `goal_src/jak1/kernel-defs.gc:597,601@64d72d1`; registered in `game/kernel/common/kmachine.cpp:1232-1234@64d72d1`. Both called from the REPL; PNG written. |
| `screen-shot-settings` declared, not defined in Jak 1 | verified | `goal_src/jak1/kernel-defs.gc:599@64d72d1`. Workaround: `rig.gc` defines it with the layout of `ScreenShotSettings` (`game/graphics/screenshot.h`: `s32 width, height, msaa; char name[244]`), mirroring `goal_src/jak2/pc/debug/capture-pc.gc:15-24`. Registering it works: the file comes out as `rig-village1-hut.png`. |
| (new) Registering is optional | verified | `game/graphics/screenshot.cpp:9@64d72d1`: default `{1920, 1080, 8, "screenshot"}`. Bare `(pc-screen-shot)` wrote `screenshot.png` at 1920x1080. |
| PNGs go to per-user config `screenshots/`, `--config-path` moves it | verified (runtime) | `common/util/FileUtil.cpp:108-115,787-796@64d72d1`. Actual path: `<config-path>/OpenGOAL/jak1/screenshots/<name>.png` (note the extra `OpenGOAL/jak1`). |
| nREPL on 8181, header `{u32 len, u32 type}`, EVAL=10, no result returned | verified (runtime) | `common/repl/config.h:36`, `common/repl/nrepl/ReplServer.h:8-13`, `ReplServer.cpp:44-54,128-179@64d72d1`. Little-endian on x86. Server sends a greeting on connect, nothing after an EVAL. One message is read per 50 ms poll (`goalc/main.cpp:172-187`). The original specification's `goalc/main.cpp ~L172` is the consumer loop, not the protocol. |
| (new) `(lt)` required before game evals | verified (runtime) | `goalc/compiler/Compiler.cpp:110,123@64d72d1`: code is only sent when `m_listener.is_connected()`. A `RIG:PRELT` marker sent before `(lt)` never appeared in gk's stdout. |
| (new) Game types need `(mi)` in the same goalc session | verified (runtime) | Without it: "The symbol *time-of-day-proc* was looked up as a global variable, but it does not exist". `(mi)` rebuilds 546 targets, 13-43 s. |
| `(start 'play (get-continue-by-name *game-info* "village1-hut"))` | verified (runtime) | `engine/target/logic-target.gc:1280`, `engine/game/game-info.gc:88@64d72d1`. |
| Wait for `(-> (level-get *level* 'village1) status)` = `'active` | refuted as a readiness signal | Works as an expression, but village1 is already `'active` during a `-debug` boot (`engine/level/level.gc:986@64d72d1`: `(('play) (if *debug-segment* 'village1 'title))`), so it is true before `start`. `rig-ready?` also checks the target's `state-time` against the start time, `target-stance`, `movie?`, blackout. |
| `(set! (-> *time-of-day-proc* 0 hour) 12)`, `time-ratio` 0.0 freezes | verified (runtime) | `engine/gfx/mood/time-of-day-h.gc:40-58`, tick at `time-of-day.gc:65-68@64d72d1`. Used `set-time-of-day` (`time-of-day.gc:158`), which also zeroes minute/second. Marker: `hour=12 minute=0 ratio=0.0000`. |
| `(set-master-mode 'pause)` pauses | verified, with a catch | `engine/game/main.gc:81-115@64d72d1`. It also draws a red PAUSE banner (`main.gc:505`: skipped only when `*cheat-mode*` is `'camera`); the rig sets that. Debug builds also draw the git sha (`main.gc:426`, `pc/pckernel-h.gc:273` `*display-sha*`); the rig clears it. |
| `(hide-hud-quick)` hides the HUD | verified, with a catch | `engine/ui/hud-classes.gc:1460@64d72d1`. It sends events; the HUD processes must run a few frames before pausing or parts stay drawn. Same for the time-of-day palette. |
| gk flags `-debug`, `--config-path` | verified (runtime) | gk logs `Overriding config directory with: ...`; settings file created under it. |

Reproducibility (two or three cold starts each; `uv run --with pillow`):

| Variant | Pause timing | Differing px | >8/255 | Mean abs diff |
|---|---|---|---|---|
| driver-timed (nREPL sleeps), run 1 vs 2 | 10 frames apart | 17.5% | 3.0% | 0.95 |
| driver-timed, run 1 vs 3 | 165 ticks apart | 77.5% | 6.8% | 2.10 |
| in-game sequencer, settle 3 s, 1 vs 2 / 1 vs 3 | same tick | 93.9% / 78.4% | 19.7% / 14.1% | 4.91 / 3.61 |
| in-game sequencer, settle 5 s (**deliverable**), 1 vs 2 | same tick (304500) | 88.9% | 17.5% | 3.72 |

In the best pair (driver-timed 1 vs 2) static tfrag/tie geometry was bit-identical; all
differences were swaying palms, Jak's idle, the orbiting sky objects and clouds. The
in-game sequencer pauses at the same game tick, yet the whole frame shows sub-pixel edge
differences, i.e. the camera differs slightly: game time in the PC port advances by real
elapsed time, not a fixed step per frame, so equal ticks do not mean equal simulation.

## Implications for the design
- The capture rig concept works end to end with no engine changes; capture rig can build on
  `capture.py`/`rig.gc`.
- Golden-image comparisons (FLIP ≈ 0 parity) cannot use this as is. They need either a
  frame-locked simulation step for captures, or to be restricted to static layers, or the
  lightbench replay path (capture DMA once, render many times).
- Readiness must be positive evidence of *our* spawn, not level status.

## Follow-ups
- Determinism: find the PC-port time step (`*display*` `time-adjust-ratio`, frame pacing)
  and force a fixed step during captures; freeze wind/foliage and sky animation; re-measure.
- Replace `(mi)` with a cheaper type load if one exists (it dominates run time).
- Exercise a continue point whose level is not resident at boot.
- Fold `tools/rig` into the `sandover` uv project once that lands.
