---
title: In an XR session the game clock steps at the XR tick, not at the flat frame rate setting
date: 2026-10-28
issue: ""
status: partly verified (unit test, flat-screen rig; headset run pending)
sha: fix/xr-game-rate
---
## Answer (≤5 lines)
While an OpenXR session runs, the game's vsync comes from a fixed 60 Hz tick, but GOAL sized
every frame as 1/target-fps. At a flat frame rate of 120 each game frame therefore claimed 1/120 s
while 1/60 s passed. The game now asks the engine for the rate its vsync really runs at
(`pc-get-xr-game-rate`: 60 during a session, else 0) and `set-time-ratios` uses it, as do the other
readers of the frame rate. Flat-screen timing is unchanged.

## Evidence
### What happened
- High frame rates on the flat screen work in two halves that must agree: the engine's frame
  limiter wakes the game `target_fps` times a second, and `set-time-ratios` (display.gc, 'custom
  video mode) gives each frame 1/target-fps seconds of game time (`seconds-per-frame`,
  `time-factor` = 300/fps ticks of the 300 Hz game clock).
- In a session the limiter is off: `xrWaitFrame` paces the render loop at the headset's rate and
  `GLDisplay::xr_game_tick` hands the game a vsync only every 1/60 s. The second half did not
  know: with the setting at 120 each 60 Hz frame still claimed 1/120 s.
- What the player gets depends on the lag compensation from the HDR-present pacing change (`display-frame-start`,
  drawable.gc). It measures the wall time since the last frame in units of the expected frame
  (1953.125 x `time-factor` timer ticks) and steps the game that many whole frames, carrying the
  rest. At 120 every XR frame measures 2.0, so the game advances at the right average speed but
  every frame is a two-step "lag" frame (physics with doubled dt, `integral-frame-counter` + 2).
  On a 90 Hz display the 60 Hz tick lands every 1.5 display frames, the measured gaps alternate
  1.33 and 2.67 expected frames, and the carry turns that into 1 and 3 steps alternating
  (computed from the code, not measured). With `*fixed-frame-step*` on (the rig) there is no
  compensation and the game runs at 60/120 = half speed.
- The flat-screen rig shows the same compensation at work: the rig instance at frame rate 120
  draws about 65 frames a second, the game clock keeps wall time (1.00) with 1.9 steps per
  frame, and with the fixed step on it advances 0.43 s per second.

### The fix: the game rate follows the XR tick (60), the tick does not follow the setting
- Engine: `Gfx::kXrGameHz` (60) is the tick period in `xr_game_tick` and the value of
  `Gfx::g_xr_game_hz` while the display is XR-paced (set where the window switches to XR pacing),
  0 otherwise. `pc-get-xr-game-rate` returns it; `Gfx::game_fps()` is the engine-side rate.
- GOAL: `pc-game-fps` (pckernel-h.gc) is the XR rate when non-zero, else `target-fps`.
  `set-time-ratios` takes the 'custom branch with that rate during a session, whatever the video
  mode (a PAL 50 setting would otherwise run 1.2x fast), so a session frame is exactly an NTSC
  frame: 1/60 s, time-factor 5. The other readers that scale per-frame work by the rate use it too:
  the button history depth for high frame rates (pad.gc), the swamp balance platform gate (swamp-obs.gc),
  the glow boost (`GlowRenderer`, now also back to 1 when the rate drops) and the fake VAG clock
  (`srpc.cpp`). The menu, the settings file, vsync and the limiter keep reading the setting itself:
  the row's value never changes behind the player's back.
- The lag compensation in `display-frame-start` measures the frame against `300 / pc-game-fps`
  rather than last frame's `time-factor`, so the first frame of a session (or after it) is not
  measured against the old rate (one extra step).
- Why not make the tick follow the setting: the tick can hand the game at most one vsync per
  display frame, and the PSVR2 runs at 90 or 120 Hz. A 165 or 144 setting (the desktop monitor's)
  would still be faster than the display, and the clock would fall back to the lag path. 60 is the
  game's native rate, below every headset rate, and the stereo path already repeats the last game
  frame on the display frames in between (`xr_repeat`).

### Tests and runs
- `sandover/tools/tests/test_xr_game_rate.py` evaluates `set-time-ratios` and `pc-game-fps` from
  the GOAL source with `pc-get-xr-game-rate` mocked: in a session, at settings 50 to 240, the
  seconds per frame equal the XR tick period (1/`kXrGameHz`, read from gfx.h); without a session,
  50/60/75/120/165 give their old values. On master's `set-time-ratios` the session case at 120
  gives 1/120 (fails). It also checks that the tick and the published rate use the one constant and
  that pad.gc, swamp-obs.gc, the glow boost and the VAG clock read the game rate.
- Full GOAL build of the branch (types-only `(mi)`, 551 targets): clean.
- Rig, flat screen, frame rate 120, this branch's gk, 5.3 s windows (`scripts/xr_game_rate.py
  --patch --no-xr`): game clock per wall second 1.00 / 1.00 (play) and 0.43 / 0.46 (fixed step)
  before / after the GOAL side was sent over the REPL: unchanged.
- Headset run not done: SteamVR refused the session at the time (`VRInitError_Init_HmdNotFound`,
  the PSVR2 driver failed to load: headset not connected). The run that proves it, with SteamVR
  on: `uv run --no-sync python scripts/xr_game_rate.py --bin <staged bin> --out <dir> --patch`.
  It measures XR windows before and after the patch; expected at 120: before, 1.0 with 2 steps per
  frame (play) and about 0.5 (fixed step); after, 1.0 with 1 step per frame in both. Exit 1 when a
  patched XR window is off by more than 5 %.

## Implications for the design
- Any new GOAL or engine code that scales per-frame work by the frame rate reads `pc-game-fps` /
  `Gfx::game_fps()`, not `target-fps`.
- If the XR tick ever becomes configurable (90 or 120 Hz game logic on a headset), only
  `kXrGameHz` changes; GOAL follows through `pc-get-xr-game-rate`.

## Follow-ups
- Pending: the headset run above (one launch, about two minutes); no HMD was connected when the
  fix was made and reviewed.
