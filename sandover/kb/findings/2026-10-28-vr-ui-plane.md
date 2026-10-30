---
title: VR 2D layer on a head-locked quad 2 m ahead (OpenXR quad layer)
date: 2026-10-28
issue: VR play check 2026-10-28 02:30: the 2D layer in the eye images
status: verified (real SteamVR session with fake pose, emulated session, flat parity)
sha: master 6e8a34715 + feat/vr-ui-plane
---
## Answer (≤5 lines)
In XR the 2D layer (HUD sprites, the 3D HUD icons, the `debug`, `debug-no-zbuf` and `subtitle`
buckets: menus, HUD text, subtitles, letterbox, blackout) is drawn into its own 1920x1080 canvas
and submitted as an `XrCompositionLayerQuad` in VIEW space, 2.0 m ahead, 55 degrees wide (2.08 m;
height from the game's aspect, 1.17 m at 16:9). The runtime composites it over the eyes. The eye
images no longer contain any 2D. Flat rendering is untouched (pixel-identical rig frame).

## Mechanism
- Before: the 2D buckets drew into each eye's frame at fixed screen positions, after the scene.
  Every eye showed the same 2D image at zero parallax: the eyes converge at infinity on it while
  the world in front of it sits nearer, and it is spread over the eye's full ~114-degree frustum.
- Now (`OpenGLRenderer::ui_layer_begin/finish`, `ui_canvas_draw` in `BucketRenderer.cpp`): the
  first eye's pass draws the 2D draws twice, once into an FP16 canvas cleared to black and once
  into one cleared to white, without any eye transform. The second eye skips them (DMA consumed).
  A resolve pass writes premultiplied colour = the black canvas and coverage
  a = 1 - mean(white - black) into the runtime's UI swapchain image. That recovers exact
  premultiplied alpha whatever blend each draw used (alpha blend, additive glow, PS2 alpha 128 =
  1.0) without touching any 2D renderer's blend state: a pixel drawn over background B ends at
  c*a + B*(1-a), so B=0 gives c*a and B=1 adds (1-a). An additive draw adds c on both, which
  cancels, so its coverage is 0; that needs the canvases in FP16 (on RGBA8 the white canvas clips
  at 1.0 and additive light read as coverage, darkening the world behind every HUD glow).
- Encode: the resolve runs the eyes' SDR tonemap (`tonemap_sdr.glsl`, shared with
  `post_processing.frag`) on the canvas colour, `T(b/a)*a` for covered pixels and `T(b)` for pure
  additive light, as the 2D got inside the eye frame. Measured against master's in-eye HUD
  (emulated session, orb counter + subtitle, `hud-master` / `hud-branch`): subtitle white peak
  246-247 on both; the orb digit 201 in-eye vs 222 on the quad, the eye-crop resample of a thin
  glyph (the subtitle's wider strokes keep their peak).
- HUD sprites (Sprite3 group 1) are held back (`defer_hud_sprites`, the motion-blur mechanism)
  and flushed into both canvases at the `debug` bucket; the 3D HUD icons (Generic2 `uses_hud`
  draws, orb/cell counters) go into both canvases where they draw.
- `OpenXr.cpp`: a third swapchain (1920x1080, the eyes' format), acquired with the eyes and
  submitted after the projection layer with `BLEND_TEXTURE_SOURCE_ALPHA` (premultiplied). With no
  UI swapchain (creation failed) the old in-eye path runs unchanged.
- Aspect: the chain's projection with the XR head-view widening undone
  (`FrameTargets::game_tan_x/y` over `view_tan_x/y`, howto/xr.md), so 256 p11 / (112 p00) of the
  game's own projection (PAL y-pix kept). Real session: the chain's tangents were 4.165 x 2.480
  (ratio 1.68, changing with the pose-age margin); the canvas stayed at 1.7778 throughout.
- Preview: the same quad is projected into the side-by-side frame (window and screenshots) from
  the eye's offset (`head_x_m`) and the drawn half-tangents, so rig frames show the plane. The
  swapchain eye images (`-left`/`-right` PNGs) carry no 2D; the runtime adds the quad.
- Quad layer picked over drawing the quad per eye in our pass: the 2D buckets are the last three
  buckets plus two separable draw groups, so separating them was not blocked. The runtime
  samples the canvas once at display resolution with its own reprojection, so the text stays
  sharp at any render scale and costs no eye-frame pixels.

## Evidence
- Flat: rig shot `village1-hut` 12 h with the Options menu open and a subtitle line, master vs
  branch: max diff 0, 0 differing pixels (1920x1080). `$SANDOVER_DATA/vr/ui-plane/2026-10-28/rig-base`, `rig-new`.
- Emulated session (PSVR2 fov, IPD 61 mm), fake head yaw 0 and +30 degrees with the menu and
  subtitle: inside the quad rect the best horizontal alignment of the yaw +30 frame onto yaw 0 is
  0 px (49148 identical pixels), while 97.9 % of the frame changed (world moved).
  `$SANDOVER_DATA/vr/ui-plane/2026-10-28/emu1` (crops `crop-*-quad-yaw0-vs-yaw30.png`).
- Real session (SteamVR 2.18, PSVR2 connected, not worn; fake pose; session VISIBLE, never
  FOCUSED), at the first width (65 degrees): ui swapchain 1920x1080 created, quad layer
  2.548 x 1.433 m at 2.0 m in VIEW space submitted with no xrEndFrame error; yaw 0 vs +30 best
  2D alignment 0 px (49093 identical px). After review (55 degrees, FP16 canvases, encode):
  real session `real2` quad 2.082 x 1.171 m, no OpenXR call errors, best alignment 0 px (32275
  identical px); emulated `emu4` the same; flat `rig-base4` (master 6e8a34715) vs `rig-new4`
  0 differing pixels.
  `$SANDOVER_DATA/vr/ui-plane/2026-10-28/real1`. Flat re-check on master 6e8a34715: `rig-base3`
  vs `rig-new3` 0 differing pixels.

## Limits
- Blackout and letterbox are 2D draws: they cover the quad only, not the whole view.
- The debug bucket's 3D debug draws (spheres, lines) also land on the plane (debug only).
- The canvas is redrawn every display frame, also on redraws of a game frame (cheap: 2D only).
- While a canvas is bound, `render_fb_depth_tex` and the MSAA facts still describe the eye
  target; no 2D renderer (DirectRenderer, Sprite3's HUD flush, Generic2's HUD draws) reads them.
- The 2D draws run twice in eye 0. None has per-call side effects beyond the profiler's draw
  counts (doubled for those buckets); DirectRenderer's state resets per call, the HUD sprites
  are flushed with `keep` the first time.
- XR screenshots also save `<name>-ui.png`: the UI image as the runtime gets it (alpha dropped).
