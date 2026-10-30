---
title: The black band over the headset sky, and the far scenery it hid
date: 2026-10-28
issue: ""
status: verified (synthetic eyes, emulated session); headset confirmation pending
sha: fix/vr-sky-band
---
## Answer (5 lines)
The band was the sky's own floor. Jak 1 draws the sky dome in three passes: the textured roof, two
cloud layers, then four flat, untextured "base" triangles that fill everything below the horizon.
The base triangles go through `direct_basic.vert`, the one direct-renderer shader without the
`xr_gs` reprojection, so in each eye they stayed in the game's screen space. The eye frame is 2.8
times taller than the game's, so the game's horizon (tangent 0.25) landed at 0.71 in the eye, fixed to the image.

## What the GPU was doing
- The game builds the sky on the EE (`render-sky-tng`, mips2c `render-sky-tri`/`render-sky-quad`):
  each polygon is transformed by `camera-temp`, clipped against the guard band
  (`clip-polygon-against-{positive,negative}-hyperplane`) and written as GS screen
  coordinates with a constant depth: 0 for roof and clouds, 256 for the base (`set-sky-vf23-value`).
- In an XR eye the C++ side reprojects GS-space geometry with `xr_gs.glsl`: back to the game
  camera's ray, through the eye's rotation, into the eye's asymmetric frustum. The roof and clouds
  (textured, `direct_basic_textured.vert`) did that. The base (untextured, `direct_basic.vert`) did not.
  It drew where the game put it, stretched over the whole eye frame (eye scale 0.452 x 0.353).
  It covered the eye from 0.71 down to the bottom and stayed there under pitch and roll.
- The far scenery was not culled: per eye the baseline and the fix submit the same draws
  (sky 20, tfrag 55-67, tie 96-187, shrub 9-32 at the four cameras, identical in both builds).
  The far islands and spires lost their upper parts exactly where the stretched base lies; with
  the sky bucket skipped (`SANDOVER_BG_PROBE_SKIP=3`) they show the same cut-off outline
  against the clear colour, so their faded, fogged edges go to whatever lies behind them, and
  behind them was black. With the sky behind them again they are back (far agreement below). The band itself is
  black in every capture (0, 0, 0); why the base comes out black there was not traced.
- Bisect on the fixed build: removing only the `direct_basic` uniform call brings the band back
  unchanged (coverage 0.488); the direction path below is not needed for it.

## Fix
1. `direct_basic.vert` goes through `xr_gs` like the other GS-space shaders, and
   `DirectRenderer::update_gl_prim` sets its uniforms. With XR off the uniform is 0 and the
   expression is the old one exactly (`(p, 1)`), so the flat path is unchanged.
2. The guard band still ends the dome a little past the game's frustum. With the head pitched 10
   degrees the reprojected sky stopped short of the top of the eye (a black strip), and looking
   further away loses more. In XR, `draw-large-polygon` now skips the clip and writes each vertex
   as its homogeneous GS position: the octahedral direction in GS x/y, log2 of the length in z bits
   9-22 plus F, the original depth in z bits 0-8 and z bit 23 as the marker.
   The length matters: the dome's corners sit at different distances (roof apex 1.0e7, corners
   4.1e7), and perspective-correct texturing needs the true w. A first version without it
   textured the clouds visibly wrong (agreement with mono 0.65 instead of 0.95). `DirectRenderer`
   decodes the marked vertices to floats, and `xr_gs` mode 3 projects them into the eye.
   GL clips the triangles in 3D, so any head direction works. Without head tracking mode 3 uses
   the identity eye; with no frame camera yet the triangles collapse and draw nothing.
   `st` goes out undivided (`q` = 1), so the texture is perspective-correct in the eye.

## Proof (synthetic eyes, `SANDOVER_XR_EMULATE`, PSVR2 profile, HDR presenter as the play launcher)
The headset was not connected, so the session was emulated (`howto/vr-synthetic.md`): the engine
path from `begin_frame` on is the real one, with the PSVR2's stored views (fov 61.50 / 43.45 /
53.04, IPD 61 mm, 3400x3468) and two textures in place of the swapchains. The emulated baseline
(master + the emulation, no fix) shows the band exactly as the headset did: black from tangent
0.71 down to the terrain at 0.30, upper edge fixed under pitch.

Data: `$SANDOVER_DATA/vr/synthetic/2026-10-28/` (`final-mono`, `final-base`, `final-new`, sheets
and JSON there). `sandover-vr-synth sky`: the mono shot warped into each identity-head eye,
compared above the horizon (the horizon from the shot's camera rotation). Coverage is the share of
pixels that are not black. Agreement is the share within 48 levels of the mono pixel; "far" is the
first 6 degrees above the horizon.

| camera | coverage mono | baseline eye | fixed eye | agreement baseline / fixed | far agreement baseline / fixed |
|---|---|---|---|---|---|
| village1-hut | 1.000 | 0.488 (-51.2 %) | 1.000 (+0.0 %) | 0.43 / 0.95 | 0.67 / 0.95 |
| beach-start | 1.000 | 0.781 (-21.9 %) | 1.000 (+0.0 %) | 0.77 / 1.00 | 0.78 / 1.00 |
| jungle-start | 0.991 | 0.940 (-5.1 %) | 0.996 (+0.5 %) | 0.93 / 0.99 | 0.98 / 0.99 |
| sunken-start (indoor) | 0.843 | 0.846 (+0.3 %) | 0.846 (+0.3 %) | 1.00 / 1.00 | 1.00 / 1.00 |

Left eye shown; the right eye is within 0.002. The test (|coverage difference| under 2 %) fails at
the three outdoor cameras on the baseline and passes at all four on the fix. Above the game's frustum
(no mono data) the fixed eyes are covered 0.99-1.00 outdoors. At pitch 10 the clipped path
(`SANDOVER_XR_SKY_CLIPPED`) leaves a black strip along the top of the eye where the guard band ends;
the direction path fills it. A first direction encoding without the vertex length agreed only 0.65
at village1-hut (clouds textured with the wrong w); with the length, 0.95 (the clipped path also gives 0.95).
Sheets: `ab-base-vs-new.png` (top four rows baseline, bottom four fixed: mono, left, right,
anaglyph), `final-{base,new}/sky.json`.

Geometry tests (a)-(e) (`sandover-vr-synth measure`) on the fixed build: identity, yaw, pitch and roll 10 degrees register
with zero residual angle, scale 1.00 and shift (0, 0) in both eyes; disparity xL - xR is 0 at the
sky and +8 to +12 px in the bottom third at all four cameras; the negative control (identity
prediction against the pitch-10 image) is off by 232 px as expected (`final-new/measure.json`).

## Not covered
- The real session on the PSVR2 (headset disconnected during this work). The emulation shares
  everything after `xrLocateViews`; the runtime's own views are the stored profile.
- Sprite distort and the HUD stay as listed in `howto/xr.md`.
