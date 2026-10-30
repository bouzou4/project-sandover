---
title: Rig frames with a black sky and no world: buckets drawn into the wrong framebuffer
date: 2026-10-23
issue: ""
status: fixed
sha: fix/vr-synthetic-rig
---
## Answer (5 lines)
Fixed: the leaking pass is `motion_vectors` `ensure_targets` (FBO 75 here, on the
per-shot resize), and the bucket loop now binds the render target before bucket 0; details in
`2026-10-27-vr-synthetic-rig.md`. The diagnosis as written on 2026-10-23 follows.

On a black rig frame the sky, tfrag and tie are submitted (20 / 43 / 35 draws on the shot frame)
but every bucket from 0 on starts with a draw framebuffer that is not the frame's render target
(FBO 75 bound, render target 105). They draw into another FBO; actors and HUD that bind the target
themselves still land. Trigger: windowed slots (`display_mode` 0) present a 320x240 window, so the
4K screenshot frame resizes every target that frame. 9 of 9 windowed shots failed (deterministic).

## Probe
`SANDOVER_BG_PROBE=1` (gk): at each bucket start it reads `GL_DRAW_FRAMEBUFFER_BINDING`
and compares it with `render_fb`; after each bucket it counts profiler draw calls per category.
It logs a `bgprobe:` line on screenshot frames and on frames with buckets off target (200 max).

## Run
Slot 6 set to `display_mode` 0, `fsr3-stills.yaml --play-look --only sky-sea-high`, 3840x2160,
primary checkout as project root, one gk. Data: `$SANDOVER_DATA/experiments/rig-black-world/probe1/`.
- PNG: top third mean 0.0, 69.5 % of pixels below 4/255 (sky and world gone).
- `bgprobe: frame 2657 shot true fb 105 3840x2160 | sky 20 tfrag 43 tie 35 merc 10 sprite 411 |
  off-target buckets: 0:75 1:75 ... 18:75`: submitted, not missing; wrong target.
- The 320x240 frames between shots also start every bucket on FBO 0 (window), so the windowed
  slot's normal frames are off target too; the 0x8ce0 `glDrawBuffers` errors in tfrag/tie/merc
  at boot are the same state (MRT draw buffers set while FBO 0 is bound).

Full set (`win1/`, 8 shots, same boot): every shot frame logs buckets 0-18 on FBO 75 and every
PNG has a black sky and no tfrag ground; water, shrubs, some tie, actors and particles remain
(top-third mean 0.0-141, black share 6.7-69 %: the mean alone misses the bright-water shots, so
the test is the probe line plus a black sky band, not a mean threshold).

## Mechanism (located, not yet named)
`setup_frame` binds the render FBO, then several passes run before bucket 0 (loader, FSR 3 /
FSR 4 / DLSS begin and release, stencil marks, SSAO normals/emissive attach, motion vectors,
actor-shadow prepass, heightfield and canopy prepare). One of them leaves another FBO bound.
Nothing in buckets 0-18 rebinds the target, so the sky and background go there. FBO ids shift
when the targets are reallocated (a size change every shot frame on a 320x240 window), which is
why fullscreen slots at the shot size rarely show it. The memory-pressure, project-path and
first-shot correlations are not needed to reproduce it.

## Fix plan
1. Bind `render_fb` (draw and read) right before the bucket loop in `dispatch_buckets_jak1`:
   the frame never starts its buckets off target, whatever a prepass leaves.
2. Name the leaking pass: extend the probe to read the binding after each pre-bucket stage and
   fix that pass's restore.
3. Rig: seed slot `display-settings.json` to `display_mode` 2 at boot (as slots 0-2), so
   normal frames match the shot size.
4. Proof: windowed slot, fsr3-stills 8 shots x 3 boots, zero frames with the top third black and
   zero `off-target` probe lines; the old build fails the same test on the first shot.
