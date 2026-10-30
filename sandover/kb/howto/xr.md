# Playing Jak 1 on a headset (OpenXR)

How to play with head tracking, what you should see, and what does not work yet. Runtime
setup (which OpenXR runtime is active, the SteamVR null driver) is in `howto/vr.md`.

## Launch

1. Start the headset's OpenXR runtime (for example SteamVR) and wait until the headset is ready.
2. Check the active runtime (`howto/vr.md`, "Before any test").
3. Start the game with `gk --xr`, or boot normally and run `(pc-set-xr #t)`, or tick
   Renderer Debug → **Stereo** → **OpenXR**.
4. The log shows `[xr] state IDLE -> RUNNING`, then `[xr] head tracking on` and
   `[xr] recentred: yaw ... deg, head at (...) m`.
5. Put the headset on. SteamVR keeps the session `SYNCHRONIZED` (no frames drawn, the timing
   line reads `submitted 0/...`) until the headset is worn, then `VISIBLE` and `FOCUSED`.
   Taking it off returns to `SYNCHRONIZED`. If SteamVR ends the session, the engine starts a
   new instance and session about 3 s later.

## Settings in VR

The headset has its own graphics settings, the VR profile (`howto/settings.md`, "Flat screen and
VR profiles"). While the session runs, Options > Sandover reads VR SETTINGS at the top and every
row and the stock MSAA row edit the VR set; the flat screen's settings stay as they were, and they
come back when the session ends. The VR set is saved in `sandover-vr.gc` next to `sandover.gc`.

- **Render scale** sizes each eye's frame: 67% by default, shown with the per-eye size (for
  example `67% (3009X2324 PER EYE)` on a PSVR2). Lower it before lowering effects when the
  headset cannot hold its display rate.
- **Upscaler** runs per eye: FSR 3.1 (default) or FSR 4, each eye with its own context and
  history, its own motion vectors (head motion included) and AO history. DLSS is not offered in
  VR. The stock MSAA row (Graphics page) only matters with vanilla rendering.
  With FSR 4 and the 2D layer, eye 0's FSR 4 runs on the D3D12 queue while GL draws eye 1, and
  both eyes come back in one interop lock (`findings/2026-10-28-vr-frame-cost.md`);
  `SANDOVER_XR_FSR4_SERIAL=1` (dev A/B) hands each eye back in turn.
- **Contact shadows** grey out: they do not run in stereo.
- **HDR output** and **HDR calibration** grey out: the eyes are SDR, and while a session runs the
  window mirror is presented through the GL swap (no HDR presenter, whose interop lock cost a
  GPU drain per display frame). The flat profile's HDR output returns when the session ends.

## Recentre

Sit the way you will play and look straight ahead, then press **F6** (or run
`(pc-set-xr-recentre)`, or press **Recentre (F6)** under Renderer Debug → Stereo). Your head
pose becomes the game camera's: its yaw and position. Pitch and roll are never taken, so the
horizon stays level. The game also recentres on its own when a session starts. The default
game bindings use no F-keys; F2 (screenshot) and F8 to F10 (snapshot capture) are taken, so
recentre sits on F6.

In a session a screenshot (F2, a play capture, `pc-screen-shot`; not an F7 bug report) also saves
both eye images and the 2D layer as `<name>-left/-right/-ui.png`: two ~47 MB readbacks that stall
the frame for about 0.3-0.6 s, which the runtime covers with reprojected frames.

## What you should see

- **Head rotation** turns your view around the game camera. The game camera still decides
  where you are and which way "forward" is; the stick and the game's own camera work as
  before and stay relative to the game camera, not to your head.
- **Small head movement** moves the eyes too: lean in, lean sideways. Past 0.5 m from the
  recentre point the view stops following. The Renderer Debug slider **Head translation**
  scales this (1 = life size, 0 = rotation only).
- **Field of view** is the headset's own, per eye. The world has its real angular size and
  you see more around the edges than on a monitor.
- **Frame rate:** the headset gets a new image at its display rate (for example 90 Hz), each
  with your current head pose. The game itself still runs at 60 Hz: animation, the game
  camera and everything that moves advance 60 times a second.
- **Tracking lost:** the view falls back to the fixed stereo pair, stuck to your head, until
  tracking returns (log: `head tracking lost: head-locked stereo fallback`).
- The desktop window shows both eyes side by side. The 2D panel is drawn into it as each eye sees it.

## Known limits

- **Object motion is 60 Hz.** At 90 Hz each game frame is shown once or twice, so moving
  things (Jak, enemies, the game camera when it pans) step slightly unevenly. Head motion
  is smooth: every display frame is drawn for the current head pose.
- **2D layer:** menus, HUD, text and subtitles are on a flat panel 2 m in front of you,
  55 degrees wide, that follows your head (an OpenXR quad layer the runtime composites).
  The blackout and letterbox are part of that panel, so they cover it, not the whole view.
- **Actors can be missing at the edge of view.** The game culls actors, particles and
  generic geometry against its own camera frustum. With your head turned, or near the edges
  of the headset's wider view, some of them can be missing or pop in. Background geometry
  is culled per eye and does not do this. Widening the game's culling is a follow-up.
- **Ocean and stencil shadows** arrive already projected by the game. They are
  reprojected into each eye from their depth, which is exact except at the edges of what
  the game drew. The **sky** is sent unclipped as directions while XR is on, so it covers
  every head direction. Water shading (reflections, depth colour) stays computed from the game
  camera, so it can look slightly off near the edge of view with your head turned.
- **Heat haze (sprite distort)** and a rare generic draw path do not follow the head yet.
- **World scale:** `stereo_world_scale` still applies to the eye distance and head
  movement (1 = life size).

## Cost

Every display frame draws the whole scene for both eyes, including the display frames
that re-draw a game frame already shown. At 90 Hz that is 1.5x the render work of drawing
each game frame once at 60 Hz. Nothing is tuned for it yet. If the GPU cannot keep the
display rate, turn on the runtime's motion smoothing (SteamVR: Motion Smoothing): it
synthesises the missing frames from the last ones.

Measured on the PSVR2 (2026-10-27, village1, all effects): SteamVR recommends 3400x3468 per
eye, the engine draws each eye at 4491x3468 (one symmetric frustum covering both eyes' fields
of view, then cropped), and a display frame takes 42 ms against the 11.1 ms of 90 Hz (at 100%
render scale). The VR profile's render scale (67% by default) draws 0.45 of those pixels; the
SteamVR per-application resolution scales the frame size underneath it. Details and the
planned savings: `kb/findings/2026-10-27-vr-bringup.md`; per-eye upscaling costs and proofs:
`kb/findings/2026-10-28-vr-per-eye-upscaling.md`.

The runtime's space warp gets nothing from the game: no motion-vector or depth swapchain is
submitted (SteamVR's motion smoothing does not use the app's vectors anyway).

With the FP16 target on, the eyes get the same tonemap as the window, without the HDR encode
(the swapchains are SDR). Brightness/contrast and the blackout fade are applied to the window only.

## How it works (for code readers)

- `game/graphics/xr/OpenXr.cpp`: per display frame, `xrLocateViews` in LOCAL; the head
  pose relative to the recentre reference becomes each eye's rotation and offset in the
  game camera's axes, plus the runtime's per-eye field of view. The layer is submitted with
  the pose each eye was drawn from.
- `game/graphics/pipelines/opengl.cpp`: while a session runs, `xrWaitFrame` paces the render
  loop; a new game chain is copied as soon as it arrives (the game is released at once) and
  the last copy is drawn again every display frame. The game's vsync is a separate 60 Hz
  clock (`xr_game_tick`).
- Re-drawn frames skip everything that advances per game frame: `frame_idx`, VIF
  interrupts, TIE wind (restored from its start-of-frame state), the actor shadow maps.
  Particles and ripples are computed by the game, so a re-drawn chain shows them unchanged.
- The game renders from the head: each game frame `pc-get-xr-cull` hands the latest head pose
  and the drawn frustum (plus a head-turn margin sized from the pose age) to `cam-update`, which
  widens the projection to it and turns `*math-camera*` (not `inv-camera-rot-smooth`, which
  steers Jak) to the head, so culling and the GS range cover the head view. The chain remembers
  the pose it was made from; each eye then applies only the rest (eye offset, head motion
  since). `kb/findings/2026-10-28-vr-eye-geometry.md`.
- The margin sets the game's projection, so it is held: it starts at 25 degrees (log
  `[xr] view margin ... held`) and follows 300 deg/s times the running mean of each chain's
  oldest pose age (located -> last display of the chain, with the runtime's display period;
  10-40 degrees, half-angle at most 80; ages over 200 ms skipped) asymmetrically: when that
  wants more than 1.5 degrees above the held margin for 0.5 s it is taken in one step; it comes
  down only after it stayed more than 6 degrees below for 2 s (frame time accumulated, stalls
  count as 0.1 s), by at most 5 degrees, at most once every 5 s. A margin that moved
  every frame made the GS snap, clipping and culling edges move every frame (jitter in the
  headset, `kb/findings/2026-10-29-vr-projection-jitter.md`). The runtime's per-eye fov and
  eye offsets are held the same way (changes under 1e-4 rad / 0.05 mm are ignored).
  `SANDOVER_XR_MARGIN_DEG` fixes the margin for A/B.
- Dev: `pc-set-xr-fake-option 8 N` (N at most 16) saves the eye images of the next N drawn
  display frames (`xrseqSS-fKK-left/-right.png` in the screenshot folder, copied on the GPU,
  saved after the last; holds 2 x N eye images in VRAM meanwhile, about 47 MB each on a PSVR2) with one `[xr] seq` line per frame; option 7 makes the fake yaw sweep (option 6) step
  a fixed angle per display frame (1/90 s of the rate) from -yaw.
- The chain's projection is then not the game's: x and y widen by different factors, which
  change with the margin. Anything that needs the game's own screen (a 2D canvas, the HUD's
  aspect) reads `xr::FrameTargets`: when `chain_head` is set, `game_tan_x/y` are the game's
  half-tangents (`x-ratio`, `y-ratio`) before the widening and `view_tan_x/y` the chain's.
  The game's 2D aspect is `game_tan_x / game_tan_y`; the game's projection is the chain's
  with `[0][0]` scaled by `view_tan_x / game_tan_x` and `[1][1]` by `view_tan_y / game_tan_y`.
  Without `chain_head` all four are 0 and the chain's projection is the game's.
  The game side does the same for `convert-to-hud-object` (orbs and cells flying to the HUD).
- The world-shadow cascades fit the drawn eye frustum (`EyeView::tan_x/y` with
  `head_chain`), not the margin: a pop at the outer edge during a fast turn is the trade for a
  near cascade as sharp as it can be.
- `VistaHiZ` (the far-level occlusion test) is off in stereo and XR (`stereo_active`).
- `EyeView` in `SharedRenderState` carries the eye into each renderer: background camera
  and frustum planes (`apply_stereo_eye`, built from the eye's own frustum), merc perspective,
  generic vertex shader, sprites and glow, and `xr_gs.glsl` for GS-space geometry.
- Temporal AA per eye: `dispatch_buckets_stereo` sets `SharedRenderState::eye_slot`; motion
  vectors, the FSR 3.1 and FSR 4 contexts and the AO history are kept per slot, and chain on
  `display_tick` (every display frame, redraws included). Each eye's camera velocity comes from
  that eye's camera at the last display frame, so head motion is in it; on a redraw of a game
  frame (`game_frame_new` false) merc takes its current bones as the previous ones, so only the
  head moves. Both eyes take the same jitter phase and cut together. Details:
  `kb/findings/2026-10-28-vr-per-eye-upscaling.md`.
- 2D layer: the first eye's pass draws the HUD sprites, the 3D HUD icons and the `debug`,
  `debug-no-zbuf` and `subtitle` buckets into a 1920x1080 canvas twice (on black and on white)
  for premultiplied alpha; the second eye skips them. `OpenXr.cpp` submits the canvas as a quad
  layer in VIEW space (`kUiDistanceM`, `kUiWidthDeg`). Details:
  `kb/findings/2026-10-28-vr-ui-plane.md`.
