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

## Recentre

Sit the way you will play and look straight ahead, then press **F6** (or run
`(pc-set-xr-recentre)`, or press **Recentre (F6)** under Renderer Debug → Stereo). Your head
pose becomes the game camera's: its yaw and position. Pitch and roll are never taken, so the
horizon stays level. The game also recentres on its own when a session starts. The default
game bindings use no F-keys; F2 (screenshot) and F8 to F10 (snapshot capture) are taken, so
recentre sits on F6.

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
- The desktop window shows both eyes side by side.

## Known limits

- **Object motion is 60 Hz.** At 90 Hz each game frame is shown once or twice, so moving
  things (Jak, enemies, the game camera when it pans) step slightly unevenly. Head motion
  is smooth: every display frame is drawn for the current head pose.
- **HUD placement:** the HUD, text and subtitles are drawn into each eye at fixed screen
  positions, so they move with your head, sit at zero parallax (they look far away) and
  can land near the edges of the wider field of view. Moving them to a quad layer is later VR work.
- **Actors can be missing at the edge of view.** The game culls actors, particles and
  generic geometry against its own camera frustum. With your head turned, or near the edges
  of the headset's wider view, some of them can be missing or pop in. Background geometry
  is culled per eye and does not do this. Widening the game's culling is a follow-up.
- **Ocean, sky and stencil shadows** arrive already projected by the game. They are
  reprojected into each eye from their depth, which is exact except at the edges of what
  the game drew. Water shading (reflections, depth colour) stays computed from the game
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
- `EyeView` in `SharedRenderState` carries the eye into each renderer: background camera
  and frustum planes (`apply_stereo_eye`), merc perspective, generic vertex shader, sprites
  and glow, and `xr_gs.glsl` for GS-space geometry.
