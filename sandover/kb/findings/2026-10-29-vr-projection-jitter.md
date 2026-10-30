---
title: Why the headset view jittered every frame with the head still
date: 2026-10-29
issue: ""
status: verified (emulated PSVR2 session and real SteamVR session with the PSVR2, fake head)
sha: fix/vr-projection-jitter
---
## Answer (5 lines)
In an XR session the game draws each frame through its own projection, widened from the
headset's field of view by a head-turn margin. That margin was recomputed on every display frame
from a running mean of the pose age, and the mean moves with every chain, so the game's projection
was different in every frame. The eye reprojection cancels the projection itself, but not what the
game quantises or clips in its own frame, so geometry edges moved by a fraction of a pixel every
frame: 10-17 % of the eye's pixels changed with the head perfectly still. The margin is now held.

## What the code did
- Since the head-view render change the game renders from the head (`pc-get-xr-cull`): `update-math-camera` sets
  `x-ratio`/`y-ratio` and the perspective's x/y scale to the drawn half-tangents widened by a
  margin. From that one value come the view planes (culling), the guard planes and the GS range
  (clipping of the ocean and shadow volumes), and the GS coordinates of everything the game
  projects itself. Each eye then maps the chain's projection back to its own.
- The margin was `300 deg/s x g_xr_pose_age_ms`, clamped 10-40 degrees, evaluated in
  `publish_xr_cull_view` on every display frame. `g_xr_pose_age_ms` is a running mean (5 % per
  chain) of each chain's age at its last display; that age alternates by a display period or more
  from chain to chain (a chain is shown once or twice at 90 Hz over the 60 Hz game, and the frame
  time varies), so the mean, and with it the margin, never stood still.
- The play log shows it: `[xr] game frustum half-tangents x 3.25 ... 4.91` sweeping up and down
  about once a second, with `[xr] view margin` cycling 13-17 degrees (pose age 43-57 ms) tonight
  and 16-25 degrees (54-85 ms) in the 15:03 session, where x sat at the 80 degree cap (5.663)
  and y swept 3.70-4.23. A line is only logged when the value moves by 2 %; the value itself
  changed every frame.
- Why a projection change that the eyes cancel still shows: the cancellation is exact only in
  real numbers. Each eye's transform is built from the chain's camera and projection and maps
  them back, in floats; the GS coordinates the game projects itself are snapped to 1/16 pixel of
  a screen that now spans a view 4-7x wider than the game's (x 3.2-5.7 against the game's 0.833);
  clip edges and the culling planes at the margin edge sit where the projection puts them. Any
  of these turns a 0.5 % projection change into a sub-pixel shift of an edge, and with MSAA an
  edge that moves by a fraction of a pixel changes its coverage. In the still sequence below the
  whole tree's outline changes every frame while the flat sky behind it does not. Which of these
  paths carries the tree was not isolated (hypothesis); holding the projection removes all of
  them at once, which the numbers below show.

## Fix
- The margin is held (`held_xr_margin_deg`, `game/graphics/pipelines/opengl.cpp`) and moves
  asymmetrically. Up at once: when the pose-age margin wants more than 1.5 degrees above the
  held value for 0.5 s, the held value takes it in one step (a frame-rate drop must not leave
  fast turns uncovered for long). Down slowly: only after the want stayed more than 6 degrees
  (20 ms of pose age) below for 2 s of accumulated frame time (a stall counts as 0.1 s), by at
  most 5 degrees, at most once every 5 s. It starts at 25 degrees. A session sees a few
  single-frame steps: emulated 25 -> 20 -> 15 in the first 7 s, then none.
  `SANDOVER_XR_MARGIN_DEG` still fixes it (logged as fixed).
- One value per frame for every consumer: the game reads the view once per game frame
  (`xr-view-fetch`), the chain carries the view it was handed (`take_xr_cull_handed`), the
  renderer derives the eye scale from the chain's own perspective (`game_camera.persp`), and the
  world-shadow cascades fit the drawn eye frustum, not the margin. With the margin held these are
  the same numbers every frame.
- Other per-frame inputs, audited:
  - `render_tan_x/y` (the drawn frustum) come from the runtime's per-eye fov each frame. They are
    now held (`OpenXr.cpp` `build_targets`): a change under 1e-4 rad is ignored, a larger one is
    taken and logged (`[xr] runtime eye fov moved`). The emulated fov is constant; the real
    session result is below.
  - `head_x_m` (the eye offset along the head axis, from the eye poses) is held the same way
    (changes under 0.05 mm ignored).
  - The predicted display period only feeds the pose age, which now only feeds the held margin's
    hysteresis.
  - The head pose itself (rotation and offset) changes with real head motion; that is the input.

## Proof (emulated PSVR2 session, `SANDOVER_XR_EMULATE`, fake head, MSAA 2x)
Test: village2 at the play run's capture 5 camera, actors frozen, game paused (nothing in the
scene moves). Ten consecutive display frames are copied on the GPU (`pc-set-xr-fake-option 8`,
no readback stall that would shift the timing), every one a new game chain. Consecutive frames
compared per eye at full resolution (3400x3468): share of pixels more than 8 levels apart (max
channel) and mean absolute difference.

| sequence | before: game half-tangents over 10 frames | before: pixels > 8 apart per pair (L / R max) | after: half-tangents | after: pixels > 8 apart (L / R max) |
|---|---|---|---|---|
| still 1 | x 3.2235-3.2381, y 2.0783-2.0851, 10 distinct | 0.167 / 0.142 (L mean 0.108), mean abs 1.0-4.0 | x 4.1653, y 2.4801, 1 value | 0.00015 / 0.00015, mean abs 0.004 |
| still 2 | x 3.2229-3.2314, 10 distinct | 0.167 / 0.143 (L mean 0.105) | 1 value | 0.00012 / 0.00012 |
Pass (< 0.1 % of pixels): before 0 of 4 eyes, after 4 of 4. The margin moved only 0.01-0.07
degrees across each before sequence; that alone moved a sixth of the picture.

Head sweeping at 60 degrees/s (option 6, stepped per display frame by option 7: 0.667 degrees a
frame), the same ten-frame capture:

| sequence | before: mean abs diff per pair, spread from median (L / R) | after |
|---|---|---|
| sweep 1 | 9.08-9.49, 2.4 % / 0.5 %, 10 distinct half-tangents | 9.06-9.29, 1.8 % / 0.5 %, 1 value |
| sweep 2 | 9.01-9.40, 3.1 % / 0.8 % | 9.07-9.30, 1.9 % / 0.5 % |
Both pass the 10 % smoothness bound; the per-frame head step dominates the difference, so the
breathing is a small share of it in motion. It is the still (or slow) head where it shows.

Coverage (the head-view head-yaw test, `eyecmp.py`, after the fix): head yaw +/-40 against the game
camera turned the same, pixels more than 48 apart 0.0003-0.0187 (8 of 8 under 0.02), coverage
equal (0.9994-1.0000) in both. The emulated margin now settles at 15 degrees instead of 10.9.

## Proof on the real SteamVR session (PSVR2 connected, not worn, fake head)
Same steps, same bins. The runtime ran at 26-36 fps here (full eye resolution, 2-3 display periods
per frame), so the pose age is longer and moves more, as in play sessions.

| sequence | before: half-tangents (10 frames) | before: pixels > 8 apart per pair, L max (mean) | after | after |
|---|---|---|---|---|
| still 1 | x 5.663 (80 degree cap), y 3.257-3.387, 7 distinct | 0.488 (0.243), mean abs 0.05-10.6 | x 5.663, y 3.279, 1 value | 0.00042 (0.00032) |
| still 2 | x 5.663, y 4.537-5.454, 10 distinct | 0.024 (0.020) | 1 value | 0.00043 (0.00026) |
| sweep 1 (60 deg/s) | y 3.921-4.882 | mean abs 8.97-9.17, spread 1.6 % | 1 value | 9.06-9.30, 1.9 % |
| sweep 2 | x 5.284-5.663, y 2.876-3.026 | mean abs 8.15-11.38, spread 22 % L / 15 % R (fail) | 1 value | 9.06-9.32, 2.2 % / 0.8 % |
Still pass (< 0.1 %): before 0 of 4 eyes, after 4 of 4. Sweep pass (within 10 %): before 2 of 4
eyes, after 4 of 4. This reproduces the play log's pattern (x pinned at the cap, y sweeping by up
to 20 % within ten frames). The runtime's per-eye fov, the eye offsets and the drawn frustum were
the same in every frame of every sequence (one value each, before and after), so on this runtime
the margin was the only moving input; the fov/offset hold is a guard. The held margin stepped
25 -> 20 -> 15 in the first 7 s and once more 15 -> 20 when the frame rate dropped (pose age
99 ms), then held through all four sequences.
Data: `real-before`, `real-after` (same layout).

Review revision (asymmetric margin, accumulated hold time), emulated, same camera: still head
0.012-0.013 % of pixels > 8 apart per pair (L and R max 0.00013, pass), 1 projection; 60 deg/s
sweep spread 1.8 % / 0.6 % (pass). Margin 25 -> 20 -> 15 at start, then held. Data: `emu-after2`.

Strip (left eye, frames 0-4 and the x16 difference to the previous frame, before and after):
`img/2026-10-29-vr-projection-jitter-still.jpg`.
Data: `$SANDOVER_DATA/shots/vr-projection-jitter/emu-before`, `emu-after` (`seqcmp.json`,
per-sequence `seq.log` with one line per frame, `diff-*.png`, `eyecmp.json`).

## Open
- Follow-up: which renderer path carried the visible per-frame shift (the tree's outline in the
  strip; candidates: the eye transform's float round trip through the chain's projection, GS
  1/16 px snapping of game-projected geometry, MSAA coverage of edges that moved sub-pixel) was
  not isolated. A per-bucket A/B (one renderer at a time on a fixed vs a moving margin, same still
  sequence) would name it and show whether any path still needs the projection to be exact.
- Turns faster than 300 deg/s for the pose age still reach past the margin at the outermost
  degrees, as before; pose prediction would shrink the age instead.
- Margin steps are single-frame changes of the same kind; with the hysteresis they happen at
  most every 5 s and only when the pose age changes for good.
