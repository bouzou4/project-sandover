---
title: Motion vectors (velocity buffer) for temporal passes
date: 2026-10-18
status: in review (engine done, proven live incl. review fixes, toggle-off parity done; cost in the combined perf pass after merge)
sha: branch feat/motion-vectors (engine commits 6f144ab53 + review fixes 340b749bd, base 9c7dd29b2)
---
## Answer (≤5 lines)
A velocity buffer works at 4K MSAA 2x: camera velocity from depth for every pixel, object
velocity for merc from the previous frame's bones (matched by process pid), a per-frame invalid
flag on cuts. Static pixels: vectors 4-10x below no reprojection (full = camera-only there by
construction). Merc: full vectors cut the error 2-4x; the edge-masked interior still reads
2.5-8.5 (x255), consistent with appearance change. Toggle off moves no deterministic pixel.

## Open items
- Cost is measured in the combined perf pass.
- Pre-existing, not from motion vectors: `GL_INVALID_OPERATION` in tfrag/tie/merc buckets after a
  load (same lines on master), and inf/NaN pixels in the FP16 scene at firecanyon (mvcheck skips
  non-finite pixels).

## What existed before
`SsaoRenderer` keeps no history: no previous matrix, no reprojection. ADR-0013's temporal
prerequisite (keep the previous view-projection) was never plumbed. The frame camera exists per
frame as `SharedRenderState::camera_*`, primed from the first tfrag bucket (`prime_camera_jak1`).

## Decisions
- Convention: RG16F, `v = p_prev - p_cur` in render-target pixels, in the texture's own texel
  grid (+x right, +y up, as GL stores it). GL consumers add `v` (UV: `v / size`); a consumer that
  flips rows (D3D interop) negates `v.y` with the flip. No jitter in `v`: a jittered projection
  subtracts its jitter delta where the previous clip position is built (the pass's `reproj` and
  merc's `mv_project`).
- Size: the render target (render-scale size below 100 %, what DLSS/FSR take). Single-sample:
  the pass reads every depth sample and keeps the nearest (foreground wins); vectors are never
  averaged across an edge.
- Camera velocity: one full-screen pass (`motion_vectors.frag`, before SMAA at the SPRITE
  bucket) with `R = M_prev * T(t_cur - t_prev) * M_cur^-1`, `M` the background camera-to-clip
  matrix (`make_new_cam_mat` plus the scissor y scale), built in double on the CPU. Depth 0 (sky)
  reprojects as a direction with the 3x3 homography of the x, y, w rows (rotation only).
- Merc object velocity: merc2 writes `(v, clip w, 1)` to an RGBA16F target at colour attachment 5
  (frame sample count), draw buffer 5 on only around merc2 draws (`SsaoRenderer::
  set_velocity_drawing`); `a = 1` makes plain alpha blending a replace; additive and
  no-depth-write draws are masked. The pass keeps the object vector where that sample's `w` is
  within 1 % of the depth's `w`, so later geometry in front wins.
- Identity: `pc-merc-draw-request` writes the drawing process's `pid` and the draw-control
  address into the spare 8 bytes of the Jak 1 water qword. Key = `pid << 32 | draw-control`;
  pids come from `next-pid` and are never reused. Only the previous frame's map is searched
  (records live one frame), so a despawned actor's record is gone the next frame; a new actor, a
  relocated process or a level change misses once and falls back to camera-only
  (`mv_cam * clip` in merc2.vert). A record is used only if it holds every bone slot the model
  sends now (LOD change). Observed: 4-48 models per frame matched, 0-2 missed.
- Cuts (frame invalid, buffer zero): camera `'teleport` and `'set-interpolation 0` call
  `pc-set-motion-vectors 4`, latched with the DMA chain in `gl_send_chain` so the flag lands on
  that frame; load blackout and the frame after; resize; first frame or a frame gap; no camera;
  jumps over 2 m or 20 degrees in a frame, or a 15 % fov change (cutscene camera animations send
  no event). Logged once per run of invalid frames (`MV cut f=.. reason=..`, `MV valid ..`).
- Screenshots (`pc-screen-shot`) rebuild the FBO; at a different size that is a resize cut, so
  shots are taken at the frame's own size (4K, MSAA 2).
- Toggle: `pc-set-motion-vectors 0` (`mv_enable`, default off), not a menu row. Off: no target,
  no pass, no uniform set; merc2 branches on `mv_on = 0`. Views: `pc-set-motion-vectors 1 N`
  (1 velocity with legend, 2 reprojection error, 3 source, 4 error with camera-only vectors).
  Dumps: `(pc-motion-vectors-dump "<dir>" N)` writes colour, velocity (+ source) and
  camera-only velocity (+ clip w) EXRs and a JSON per frame; `python -m sandover.rig.mvcheck`.

## Evidence (live, 4K, MSAA 2 + SMAA, default look; `$SANDOVER_DATA/runs/20261018-motion-vectors/`)
MAE of the warped previous colour against the current one, display-encoded x 255, disoccluded
and off-frame pixels excluded. vec = full vectors, cam = camera-only, none = no reprojection.

| Run | Pixels | vec | cam | none |
|---|---|---|---|---|
| A: beach, Jak hidden, camera orbit (5 pairs), static | 6.3M | 0.58-0.94 | same | 2.4-7.9 |
| A: sky | 1.8M | 0.19-0.94 | same | 0.5-4.4 |
| B1: misty, Jak runs and jumps 12 m from a bonelurker (5 pairs), merc | 95-109k | 3.0-9.0 | 10.4-25.7 | 11.2-27.6 |
| B1: static | 7.3M | 0.64-0.83 | same | 4.8-8.3 |
| Lap (8 levels, orbit), merc | 0.1-0.9M | e.g. snow 13.7 / 20.7 | 43.2 / 55.3 | 48.7 / 62.5 |

- A: the only merc in view (a chest) is 40k px; vec equals cam on static and sky to 3 decimals.
- C (game cut inside a dump): `MV cut f=2781 reason=game`; that frame is flagged and skipped,
  its buffer is zero; the frames either side are valid. Warps: one run of `blackout` /
  `no-camera` frames each; one `jump` (36 m, 42 degrees) caught by the heuristic.
- Lap: village1, beach, jungle, misty, firecanyon, sunken, swamp, snow: all four views shot at 4K
  (`lap1/<level>-<view>.png`), no garbage seen; HUD merc icons get object velocity (about zero,
  they are screen-fixed). Particles (no depth) show as error dots: they take the vector of the
  surface behind them.

- Merc interior (C, `mvcheck --edge N`: drops pixels within N px of a source boundary or whose
  warped previous source differs): B1 object class vec 7.9 / 8.5 / 8.4 / 2.6 / 2.6 at N = 4 (cam
  19.6 / 25.7 / 17.4 / 8.8 / 10.3), 7.7-2.5 at N = 8. Edges are not the cause. Object motion in
  the high frames is 60-260 px per frame (Jak jumping close to the camera at 4K). Global offsets
  of the vectors (+-3 px) never beat zero; per 24 px block the best local shift (+-4 px) lowers
  the mean only 8.9 -> 7.6 (frame 2063) and 2.7 -> 2.6 (2066) with mean shift ~0. So the
  residual is consistent with appearance change (pose-dependent shading, self-occlusion,
  lighting). A +-3/4 px search cannot detect a vector error at 60-260 px per frame, so this does
  not rule out a bone match miss, a one-frame lag or a wrong matrix on its own; the fixed-camera
  run (B2, below) cuts moving-merc error 2.4-5.7x against camera-only (7.0/40.2, 5.3/24.9, 11.6/28.2). Known limitation, not fixed.
- After a game cut the camera swings about 16 degrees per frame (just under the 20 degree cut
  threshold) for several frames; that is where the sky reads 20-50 for vec = cam: large rotation
  under a resampled sky, not a cut miss.

## Live runs after the review fixes (2026-10-18, build 340b749bd)
- B2, fixed camera (`proof3/`, `mv-fix-camera` now copies the camera combiner's trans and
  rotation; copying `*math-camera*` aliased or mis-pointed it): Jak strafes and jumps across a
  still camera near a bonelurker and the misty mud pool (both merc). Merc pixels moving over 10 px
  per frame (150-224 px mean, edge-masked): vectors 7.0 / 5.3 / 11.6, camera-only (= none, the
  camera is still) 40.2 / 24.9 / 28.2. Whole object class: vec 2.5-4.2, cam 6.2-7.7. Static 1.1-1.3
  (particles, mud animation). Merc previous bones matched 39-43 per frame with the per-bucket
  camera, 0-2 missed. Source view aligned with the merc silhouettes.
- D, wind sway (`proof2/D-wind`, `D-calm`): jungle-start, still camera, Jak hidden, five pairs with
  the level's wind and five with `*wind-scales*` zeroed. Static mean error 0.15-0.22 (wind) vs
  0.02-0.24 (calm), p99.9 12-16 vs 2.6-18; the mean error maps (x16) of both show the same water
  and rope shimmer and no foliage outline. Sway at play distance is below what the frame-to-frame
  noise shows: camera-only by design, no per-vertex wind velocity.
- Cut path not touched by the fixes (only its log default); not re-run.

## Parity (toggle off, 2026-10-18)
`mv-parity.yaml` (scratch): 7 continues (village1-hut, beach, jungle, sunken, misty, firecanyon,
snow) x noon/night at 3840x2160 MSAA 2, rig default look (`$SANDOVER_DATA/runs/
20261018-motion-vectors/parity-vanilla/`), master bin `20261018-010647-a6be8d8313` twice, branch
once; `pixdiff --threshold 0`.
- The rig is not bit-deterministic run to run at 4K: master vs master differs on 11 of 14 shots,
  either on merc actors only (idle animation: 0.01-0.4 % of pixels) or on the whole frame (a
  bimodal frame state, 98-99 %).
- Branch vs master: every shot is bit-identical to at least one master run except four, and those
  differ only on merc actors (Jak's silhouette, the same pixels the two master runs disagree on).
  The three shots where both master runs agree exactly (misty noon and night, village1 night) are
  bit-identical on the branch. No deterministic pixel moves with the toggle off.
- A play-look run (`parity/`, `--play-look`, SMAA on) has a larger floor (0.3-5.9 % master vs
  master; one rig flake rendered firecanyon at a wider field of view and did not repeat) and was
  not used for the verdict.
- Code read of the off path: no target is allocated and no pass runs. merc2 still carries two
  extra vec4 varyings and the location-5 output (draw buffer 5 is `GL_NONE`, `mv_on` 0); their
  cost is unmeasured until the cost row exists. The GOAL side only writes the spare water qword
  bytes and calls `pc-set-motion-vectors 4` on cuts (a flag).

## Review fixes (2026-10-18; run live in B2 and the wind run above)
- Merc previous camera per bucket: each merc bucket keeps its own setup camera for this frame and
  the last (`Merc2::MvCam`, keyed by bucket id via `set_bucket`); before, the first merc setup of
  the frame was paired with every bucket's draws, so a bucket with another camera (HUD) got the
  wrong previous projection. A bucket with no setup in the previous frame falls back to
  camera-only. Shadow replays do not touch it.
- Cut frame alignment, checked on the C dump: `move_m` 0.00 at f=2779-2780, 5.80 m (16.0 deg) at
  f=2781 (flagged `game`), 0.18 m at f=2782 (valid, static error 2.2 vs none 7.2). The game event
  lands on the frame where the camera jumps; no extra invalid frame added. The proof1 B1 warp
  caught by the jump heuristic (36 m, 42 deg) moved Jak (`mv-near`) and the follow camera in one
  nREPL message and sent `'teleport` in a later one, so the jump came before the event there.
- Debug views below 100 % render scale draw over the whole display framebuffer (viewport and UV
  from the bound target, velocity still in render pixels).
- Cut logging off by default (`pc-set-motion-vectors 8 1000000` turns it on); the incomplete
  object target error logs once.
- Static pixels take `v = cam` by construction (`motion_vectors.frag`), so "static: full =
  camera-only" is not evidence; the evidence there is vectors vs none.

## Cost
Cost: measured in the combined perf pass after merge. One branch run finished before that call
(`$SANDOVER_DATA/runs/20261018-motion-vectors/perf/`, 21 perf spots, 4K MSAA 2 + SMAA + HDR, base
MV on, toggle off; another gk instance was up for part of it, so frame marginals are noisy):
- `motion-vectors` GPU group (velocity pass + object target clear): mean 0.137 ms, 0.114-0.158 ms
  per spot, noise 0.02 ms. The merc object writes are inside `merc` and not split out.
- Frame marginal (on minus off): inside the spot's noise band at 18 of 21 spots; the three
  outside (temple-top +1.17 vs 0.51, hut-roof +0.25 vs 0.01, swamp +0.13 vs 0.07) are
  indicative only.

## Crops
`$SANDOVER_DATA/runs/20261018-motion-vectors/crops/`: `pan-*` (A, camera orbit), `merc-B1-*` (Jak
and a bonelurker, follow camera), `cut-f1..f3-*` (frame before, the flagged game cut with its
zero buffer, frame after; rendered from the dump EXRs). Each as `-full.jpg` (half size) and
`-crop.png` (full-res 1280x720); views velocity, error, source, error-cam. `merc-B2-fixed-*`
(fixed camera, from the dump and the source view), `wind-jungle-error-*` (wind on, still camera).

## Coverage
| Geometry | Method | Proven |
|---|---|---|
| tfrag, tie, shrub, hfrag, vista neighbours | camera (depth) | yes: static class, every lap level |
| ocean, inland water, lava | camera, depth of the displaced surface (waves not tracked) | ocean classed static in beach/jungle/village1 dumps |
| sky, sun, stars | rotation only (no depth) | yes: A, misty |
| merc actors, merc props, HUD merc | previous bones, camera-only on a miss | yes: B1, lap; interior residual consistent with appearance change |
| wind-animated tie/shrub | camera only, by design | yes: D, sway not measurable at play distance (wind vs calm not distinguishable, 0.15-0.22 vs 0.02-0.24) |
| generic (mercneric), sprites, particles | camera of the surface they cover or write | visible in views; not separately measured |

## Open items
- Merc interior residual (2.5-8.5 x255 at 60-260 px/frame), consistent with appearance change; a temporal pass
  needs its usual colour clamp there.
- Particles (no depth) take the vector of the surface behind them.
- Perf pass (deferred): `Merc2::MvBones` maps allocate one ~8 KB node per actor per frame;
  preallocated vectors with an index map later.
- Cost: measured in the combined perf pass after merge.
