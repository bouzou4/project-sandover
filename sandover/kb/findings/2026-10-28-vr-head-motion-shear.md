---
title: Why envmapped scenery slid and sheared with head motion in the headset
date: 2026-10-28
issue: ""
status: verified (emulated PSVR2 session, moving fake head); real-session run pending
sha: fix/vr-head-motion-shear
---
## Answer (5 lines)
Only the envmapped TIE scenery (the Tie3 `etie` path: shiny metal such as the Rock Village
pagoda's roof tiers and walls) moved with the head. Each XR eye applies the head motion since the
game frame (the per-eye delta) to the background camera, but only to its rotation part; `etie`
reads the whole world-to-camera matrix, translation row included, and that row stayed the game's.
The error is the delta angle times the distance to the world origin (about 1.5 km in village2).

## What the code did
- Since the head-view render change the game draws the chain from the head pose it was handed, and each eye applies only
  the rest: `apply_xr_eye` (`background_common.cpp`) turns the background packet's camera by the
  eye's `rot` and moves it by `t`. That delta is the eye's IPD offset when the head is still, and
  the head motion between the pose the game used and the pose the eye is drawn with when it moves
  (one game frame plus up to two display frames, 40-85 ms; 300 deg/s gives 7-8 degrees here).
- `apply_xr_eye` updated `camera` (world to clip, used by tfrag, tie and shrub), the planes, `trans`
  and the 3x3 of `rot`, but not `rot[3]`. `rot` is the game's `camera-rot`, which `cam-update`
  completes with the translation row `-trans * R` (the world origin in camera space).
- Tie3's envmapped draws (`init_etie_cam_uniforms`) upload `rot` as `cam_no_persp`, and
  `etie_base.vert` / `etie.vert` transform world positions with all four rows. So an envmapped
  piece was drawn as `R_eye * R_game * p + T_game` instead of `R_eye * (R_game * p + T_game - t)`:
  the head motion turned the object about the world origin instead of the eye. The offset is
  `(R_eye - I) * T_game`, about 26 m per degree at 1.5 km, so a slow head turn slid and sheared
  the pagoda's roof tiers off its walls (play run 20261028-1503, captures 4 and 5) and a fast one
  threw whole buildings across the view. The eyes' IPD offset was also missing on these pieces
  (both eyes drew them from the head centre).
- Debug stereo (`stereo_eye_x`) had the same gap: the eye shift moved `camera` but not `rot[3]`.

## Audit of the other paths (code read)
| Path | How the eye is applied | Affected |
|---|---|---|
| TFRAG, TIE (not envmapped), shrub | `camera` via `eye_world_to_clip` | no |
| TIE envmapped (`etie`, `etie_base`) | `rot` incl. row 3 (`cam_no_persp`) | yes, fixed |
| Merc | bones in camera space, `eye_camera_to_clip` on the projection | no |
| Generic | `eye_rot` / `eye_t` uniforms in `generic.vert` | no |
| Sprites, glow | `eye_world_to_clip` / `eye_world_to_camera` from the game camera | no |
| Ocean, shadow volumes, direct | `xr_gs.glsl` reprojection of GS coordinates | no |
| Sky | `xr_gs` modes 2 and 3 (directions) | no |
| `camera_rot` readers (lights, SSAO, motion vectors, sun disc, sprites' fog) | rows 0-2 only (directions) | no |
The per-frame change of the widened tangents does not reach a cached matrix: `game_camera` is
rebuilt from every chain's first background packet (`prime_camera_jak1` before the buckets).

## Fix
`apply_xr_eye` ends with `eye_world_to_camera(cam->rot, e)`, which turns rows 0-2 and maps the
translation row to `R * (row - t)`, so `rot` is the eye's full world-to-camera matrix like
`camera`. Debug stereo moves `rot[3].x` by the eye offset like `camera[3]`. Flat (mono) frames
take neither branch: no change.

Also: the XR screenshot (F7, `pc-screen-shot`) saves the per-eye swapchain images for a real
head too, not only the scripted one (still not into bug-report bundles), and logs the dumped
frame's eye pose and the chain's head pose (`[xr] eye dump:`).

## Proof (emulated PSVR2 session, `SANDOVER_XR_EMULATE`, fake head)
Test: at the play run's capture 5 camera
in village2, the head sweeps yaw +/-12 degrees at 300 deg/s inside the engine (fake option 6); the
eye images of a moving frame are compared with a frame shot with the head held at the yaw that eye
was drawn at (read from the `eye dump` line). Share of pixels more than 48 levels apart, pass < 0.02:

| shot | eye-vs-chain delta | before L / R | after: delta | after L / R |
|---|---|---|---|---|
| move1 | -6.9 deg | 0.024 / 0.023 | -7.1 deg | 0.005 / 0.004 |
| move2 | -8.3 deg | 0.030 / 0.034 | -6.9 deg | 0.009 / 0.008 |
| move3 | +5.5 deg | 0.033 / 0.024 | -1.4 deg | 0.002 / 0.002 |
Before: 6 of 6 eyes fail; after: 6 of 6 pass. The before frames lose the pagoda's envmapped roof
tiers and walls to tens of metres away; the rest of the picture matches the held frame.
`img/2026-10-28-vr-head-motion-shear.jpg` (move2, left eye).
Data: `$SANDOVER_DATA/shots/vr-head-motion-shear/emu-before`, `emu-after` (`cmp.json`, `diff-*.png`).

Re-run on the tree merged with master c3a011cd9 (FSR 4 hand-back once per frame), deltas
2.4-10.4 degrees:

| look | before (6 eyes) | after (6 eyes) |
|---|---|---|
| MSAA 2x | 0.025-0.040, 6 fail | 0.002-0.005, 6 pass |
| FSR 4 (deferred hand-back) | 0.025-0.088, 6 fail | 0.005-0.016, 6 pass |
The FSR 4 residual is the upscaler's history in motion (the held frame has settled).
Data: `m-before2-msaa`, `m-after2-msaa`, `m-before2-fsr4`, `m-after2-fsr4`.

## Open
- Real SteamVR session with the PSVR2: same steps, pending (headset not connected at the time).
- XR screenshot harnesses: since the VR 2D layer an XR shot also writes `<stem>-ui.png` (the 2D layer canvas,
  1920x1080, premultiplied, black where no HUD) before the main PNG. A harness that takes the
  newest PNG not ending in `-left`/`-right` picks the canvas as the shot (black) and then finds no
  `<stem>-ui-left.png`, so the eye images look missing; exclude `-ui` too.
