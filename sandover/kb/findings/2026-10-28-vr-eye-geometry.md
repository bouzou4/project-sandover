---
title: Why the headset view lost the world at the edge of a head turn, and the eye geometry audit
date: 2026-10-28
issue: ""
status: verified (emulated PSVR2 session); real-session check pending
sha: fix/vr-eye-geometry
---
## Answer (5 lines)
The eyes were already drawn with the runtime's per-eye pose and asymmetric field of view, and the
parallax is right: Jak at 17.1 m sits 2.98 px apart between the eyes against 2.91 px predicted from
the 61 mm IPD. What was bound by the game's 64 degree camera is everything the game itself
produces for the screen: its culling planes and the ocean, shadow volumes and other geometry it
projects to GS coordinates. Turn the head 40 degrees and the eye looks past that camera, so the
ocean ended in a black wedge. The game now renders from the head and culls with the drawn frustum.

## What the code did
- The renderer side was correct. `OpenXr.cpp` `build_targets` turns `xrLocateViews` into one
  rotation and offset per eye in the game camera's axes; `dispatch_buckets_stereo` draws the
  DMA chain once per eye with them (`EyeView`), the background planes are widened to the drawn
  frustum (`apply_xr_eye`), and each eye is cut from one symmetric frustum (half-tangents
  1.842 x 1.329) at its asymmetric window (`copy_eye_to_xr_cropped`). The log line
  `game frustum half-tangents ... -> eye scale x 0.234` is the ratio between the game's
  projection and the drawn one, not a crop: it cancels out (0.430 during a cutscene zoom,
  0.833 in play). The F7 frame is the side-by-side buffer, both eyes in the same symmetric
  frustum, which is why its halves look alike; the swapchain images differ by the asymmetric windows.
- The game side was not. Each game frame `cam-update` builds `*math-camera*` from the gameplay
  camera only. Its four planes (64 degrees, 16:9) decide which actors get drawn
  (`dma-add-process-drawable`: `sphere-in-view-frustum?`, then `camera z + radius > 0`, so
  nothing behind the camera plane), which particles launch and which ocean tiles are sent, and
  its projection is what the ocean's VU programs, the shadow volumes and other EE-projected
  geometry are expressed in. GS coordinates only exist in front of that camera and inside its
  guard band. `xr_gs.glsl` reprojects them into the eye, but what was never produced cannot be
  reprojected: at head yaw +40 the left eye reaches 101.5 degrees from the game's forward axis.

## Fix
1. Render thread: every XR frame publishes the head view (`Gfx::XrCullView`: the head rotation
   in the game camera's axes, its offset, the drawn half-tangents widened by a head-turn margin,
   and when the pose was located).
2. `xr-view-fetch` (start of `update-camera`) takes it with `pc-get-xr-cull`, only when the camera
   is rebuilt from the camera system this frame; the pad-driven external camera moves
   `*math-camera*` incrementally and keeps the game camera (a null buffer tells the engine).
   `update-math-camera` then widens the projection to the head view: `x-ratio`/`y-ratio` and
   the perspective's x/y scale (the 2D sprite matrix keeps the game's, so the HUD does not
   shrink; the fog correction follows the game's fov, unchanged). From that one change the view
   planes, the guard planes (still 4x the projection, as the GS range is), the GS range the
   ocean's VU programs and the shadow volumes clip to, and the world-shadow cascade fit
   (`ActorShadows` reads the tangents from the perspective) all cover the head view, also
   during cutscene zooms.
3. `xr-render-camera-update` (after the smoothed camera Jak's controls use) turns
   `*math-camera*` to the head.
4. The pose handed to the game is latched with the chain that game frame sends
   (`gl_send_chain`) and carried to the renderer in `FrameTargets::head_rot/head_t`. Each eye
   then takes only the rest: rot x H^T, H x (t - head_t). Redrawn display frames (90 Hz over the
   60 Hz game) correct the head motion since the chain the same way.
5. `apply_xr_eye` builds each eye's background planes from its own frustum instead of widening
   the game's.
6. Margin: the pose ages from `xrLocateViews` to the last display of the chain drawn from it
   (one game frame plus up to two display frames, more at slow frame rates). The render thread
   keeps a running mean of each chain's worst age, the one at its last display (the runtime's
   predicted display period added; samples over 200 ms are stalls and are skipped), and widens
   by 300 deg/s (a brisk head turn) times the age, between 10 and 40 degrees, the half-angle
   capped at 80 degrees (GS quantisation stays near one eye pixel there).
   `SANDOVER_XR_MARGIN_DEG=N` fixes it for A/B.
7. The chain's projection is no longer the game's (x and y widen by different factors that move
   with the margin). The game reports its own half-tangents (`pc-set-xr-game-tan`, its
   `x-ratio`/`y-ratio` before the widening); they ride on the chain as
   `FrameTargets::game_tan_x/y` next to the widened `view_tan_x/y`, for 2D consumers such as a
   UI canvas (`kb/howto/xr.md`). `convert-to-hud-object` (orbs and cells flying to the HUD)
   rescales its projected point by the same ratio, since the HUD keeps the game's projection.
8. The world-shadow cascades fit the drawn eye frustum (`EyeView::tan_x/y`), not the margin:
   the margin only guards fast turns, and fitting it made every cascade 1.6-2.4x coarser.
9. While the session runs but no eye is drawn (dashboard, not visible) the view is cleared, so the
   game goes back to its own camera.
Flat and debug stereo: `pc-get-xr-cull` returns 0, nothing changes.

## Proof (emulated PSVR2 session, `SANDOVER_XR_EMULATE`, fake head, 67 % eye frame, MSAA)
Test: a head yaw of +/-40 degrees must look exactly like turning the game camera by the same
angle (world fixed, nothing bound by the game camera). `eyecmp.py` compares the swapchain
images of the two shots per eye: share of pixels more than 48 levels apart, and coverage (not black).

| shot | before: differ / coverage head vs camera | after |
|---|---|---|
| village1-hut yaw +40 left | 0.032 / 0.975 vs 0.999 | 0.009 / 1.000 vs 1.000 |
| village1-hut yaw +40 right | 0.018 / 0.993 vs 1.000 | 0.008 / 1.000 vs 1.000 |
| village1-hut yaw -40 left | 0.026 | 0.008 |
| village1-hut yaw -40 right | 0.050 | 0.008 |
| village2-start yaw +40 right | 0.052 | 0.001 |
| village2-start other three | 0.010-0.012 | 0.0002-0.0004 |
Pass: coverage equal and differ under 0.02; fails 5 of 8 before, passes 8 of 8 after (the
residual is the animated ocean between the two shots). Parallax: Jak at 17.06 m (camera depth
from `RIG:CAMERA` and his position), focal 812.7 px in the shared frustum, predicted 2.91 px,
measured 2.98 px before and after.
Data: `$SANDOVER_DATA/shots/vr-eye-geometry/emu-before3`, `emu-after3` (`eyecmp.json`,
`diff-*.png`, `jak-v1-LR.png`), sheets `pr-yaw40-*.png`.

### Revision 2 (projection widened, margin from the pose age)
`geom2.steps`/`geom3.steps`, `eyecmp2.py`, `cascade_cover.py`; first PR revision `r2-pr`, this
one `r3-after`, `r4-after`, fixed 10 degrees `r3-m10`, `r4-m10`:
- Cutscene zoom (look-through camera, fov 25 degrees) with head yaw +40: first revision 6.6 % of
  the left eye's outer tenth black (the GS range followed the zoomed projection), now 0.
- Head pitch -30 with yaw +40 over the ocean: no black pixels in either.
- Held yaw +/-40 against the camera turned the same: 0.007-0.011 of pixels apart (pass < 0.02).
- Moving head, yaw swept +/-45 degrees at 300 and 450 deg/s inside the engine
  (`pc-set-xr-fake-option 6`), full eye resolution, 40-64 ms frames: measured pose age 59-65 ms,
  margin 18-20 degrees; no black at any eye edge. The fixed 10 degree arm was clean too: the guard
  band, 4x the widened projection, absorbs the GS-space part at these rates; what the margin
  buys is the culling of actors and particles, which this camera has none of at the edges.
- World-shadow cascades: fitted as `WorldShadows` does, the first revision's 64 degree tangents
  covered the left eye's receivers at 60 / 80 / 100 m view depth 87 / 71 / 53 %; the widened
  projection covers 100 % at every depth. The eye-edge shot at village1-hut (world map on vs
  off) shows the same shadow bands in both revisions: its casters are all within 40 m.

### Revision 3
`r5.steps`, `r6.steps`, `cascade_cover2.py` (tangents read from the run's log), `ramp.py`;
previous revision `r5-before`, `r6-before`, this one `r5-after`, `r6-after`, `r6-final`:
- Near cascade texel at shadow quality low (1024, 2 cascades, 120 m), PSVR2 eye
  (tangents -1.842 / 0.947 x +/-1.329): game camera 0.038 m (flat); previous revision
  0.143-0.221 m (margin 10-16 degrees); now 0.091 m. Far cascade 0.23 / 0.86-1.33 / 0.54 m.
  The left eye's receivers stay 100 % covered at 5-100 m. The rest of the gap to flat is the
  eye's real width (123 x 106 degrees against the game's 80 x 50).
- Shot, village1-hut, quality low: the plank's cast-shadow edge (world map on / off mask,
  10-90 % ramp over 350 columns) 33 px before, 21 px after (pass < 25).
  `img/2026-10-28-vr-eye-geometry-shadow-low.jpg`.
- Margin after the level load: the previous revision went 12.9 -> 16.8 degrees right after
  the stage (stall samples clamped and kept); now it stays at its start and settles to 10.9.
- The game's own tangents arrive on the chain: x 0.833 y 0.469 with the view at 2.99 x 1.97.
- `VistaHiZ` (far-level occlusion) is off in stereo and XR (`!stereo_active` in both
  `prepare_hiz` and `render`): no per-eye depth mismatch is possible.
- `xr-view-fetch` now predicts the look-through count as `update-camera` leaves it: a frame
  where it drops 1 -> 0 without a combiner is moved from the pad (incrementally) and must not
  take the head turn on top, which drifted the camera by one head rotation.

## Open
- Headset check: the ocean and shadow-volume geometry still snaps to 1/16 GS pixel, which now
  spans a ~3.6x wider view (about two snap steps per eye pixel at the edges); look for
  shimmer on the ocean edge.
- Real session on the PSVR2 (SteamVR was off during this pass).
- Parallax is physically right but small at the third-person camera distance (Jak 13-17 m away:
  3-6 px of 4491). How strong the depth reads is the world scale, a look decision.
- Cost: the widened view culls less; not measured here (perf after the look is approved).
- The near cascade is still 2.4x the flat texel (the eye is that much wider). A tighter fit
  (light-space box of the slice instead of its bounding sphere, or a nearer first split in XR)
  is the next lever if the headset shows it.
- Past the margin (turns faster than 300 deg/s for the pose age) the outermost degrees can miss
  actors and GS-space geometry; pose prediction (extrapolating the head to the chain's display
  time) would shrink the age instead of widening the view.
