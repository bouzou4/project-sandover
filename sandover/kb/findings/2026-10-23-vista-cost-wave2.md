---
title: Neighbour-level (vista) cost, wave 2: attribution and exact occlusion by the viewer levels' depth
date: 2026-10-23
issue: ""
status: hi-Z occlusion shipped (vista option 15, default on); draw merging, LOD and per-pass skips not pursued (below)
sha: opt/vista-wave2 on master ecd4d45e6
---
## Answer (≤5 lines)
Neighbour draws left out when their bounds lie wholly behind the depth the viewer levels drew
first (`VistaHiZ`) save 3.3 ms at village1-night and 3.6 ms at village1-hut-lamps, and cost
0.5 ms at open-sea-low, where nothing is hidden (same boot, master ecd4d45e6, 4K). The output is
unchanged by construction and measured so: drawing only what the test leaves out passes 0 samples
at every spot. The remaining overhead is the depth reduction pass (1.4-2.1 ms GPU), the next lever.

## 1. Setup
- Build: `opt/vista-wave2` (gk only, the shared worktree recipe), staged binaries and a staged
  project root per `rig-capture.md` "Worktree builds". Runs r1-r3 on master 4a078e70a plus the
  branch; the final run on the branch merged with master ecd4d45e6 (the stencil, porosity-table and receiver changes cut the
  frame by about half at these spots, so only the final run's numbers compare with today's master).
- Tool: `sandover-perf` (`shots/perf.yaml`), 3840x2160, the game's Sandover defaults after the
  load (FSR 3.1 native temporal AA), 60-frame windows, one instance, no other gk and the GPU 3D
  engine idle before each boot, both build lanes held. Every marginal below is in-boot (A/B/A).
- Spots: the three with the largest `vista` group in `2026-10-22-perf-rdna4.md`: open-sea-low,
  village1-hut-lamps, village1-night.
- Data: `$SANDOVER_DATA/perf/vista-wave2-{r1,r2,r3,final}`, captures
  `$SANDOVER_DATA/shots/vista-wave2-*`.

## 2. Attribution (run r1, master 4a078e70a, hi-Z on in the base)
GPU ms; `fragments` = what scissoring every neighbour draw to one pixel removes from the `vista`
group (probe: vista option 14 bit 1); `hidden` = share of tested vis nodes, TIE groups and shrub
chunks whose bounds lie wholly behind the viewer levels' depth or off screen.

| spot | vista off saves (frame) | vista group | tie / tfrag / shrub | per neighbour | draws, ktris | fragments | alpha pass | shrub off | hidden |
|---|---|---|---|---|---|---|---|---|---|
| open-sea-low | 11.1 | 12.2 | 7.4 / 1.6 / 1.2 | village1 4.9, beach 2.0, village2 1.0, rolling 0.8, firecanyon 0.8, jungle 0.7 | 2284, 3418 | 6.9 (frame 0.5: the ocean shades those pixels instead) | 0.04 | 1.25 | 0.07 % |
| village1-night | 10.6 | 11.5 | 7.2 / 1.8 / 0.6 | beach 4.4, jungle 3.0, rolling 1.0, firecanyon 0.9, village2 0.3 | 1345, 2217 | 6.4 (frame 6.9) | 0.07 | 0.60 | 51 % |
| village1-hut-lamps | 1.5 (5.7 with hi-Z off) | 2.7 | all culled | beach 5.7 with hi-Z off, 0 with it on | 16, 3 | ~0 | 0.00 | 0.08 | 93 % |

- TIE is two thirds of the vista group at every spot; about 60 % of the group is fragment work
  (agrees with `2026-10-10-opt-vista.md`).
- The alpha pass (trans/dirt/ice/water trees) costs under 0.1 ms: nothing to skip there.
- Overdraw by the viewer level: from the sea nothing of the neighbours is behind village1's
  geometry (0.07 %); from village1 at night half the neighbour bounds are, and inside the
  lamp hut 93 %, the whole of beach (5.7 ms) included. Early depth rejection did not make that
  hidden geometry free: its vertex, raster and per-draw work stayed.

## 3. Win: occlusion by the viewer levels' depth (`VistaHiZ`, vista option 15)
- After bucket 18 (the viewer levels' tfrag and TIE, before the shrubs and the mid ocean) the
  depth is reduced in two passes (4x4, then 2x2) to farthest-depth tiles of 8x8 pixels and read
  back through a PBO and a fence. Before the neighbour draws (after bucket 30) the CPU tests each
  frustum-visible vis node, TIE group, shrub chunk and whole level: project the bounds' eight
  corners with the shaders' own camera matrix (`make_new_cam_mat`, the scissor scale, GL depth
  mapping), pad 2 px for jitter, and leave it out when its nearest depth is below every tile it
  covers, or when it is off screen.
- Exact: depth only grows under GEQUAL, and the depth at bucket 18 is a subset of the depth the
  neighbours test against, so a node left out cannot pass the depth test anywhere. Off for trees
  with a draw that is not depth-tested GEQUAL/GREATER, in stereo and in the single-level fog modes.
- Proof (vista option 14 bit 2): the neighbours draw only what the test leaves out, inside a
  `GL_SAMPLES_PASSED` query. Result 0 samples, every frame, at all three spots, in runs r2 and
  final (`vista-hiz-leak`).
- First version read back synchronously at bucket 30: the GPU idled while the CPU caught up
  (+1.2 ms at open-sea-low). The asynchronous readback removed the wait (`vista-part:hiz-wait`
  0.00 ms; the render thread waits 1-10 ms in `vista-hiz-sync-ms` only because it runs ahead of a
  GPU-bound frame).
- Same boot, master ecd4d45e6 merged (run `final`), frame ms saved by hi-Z (negative = slower):

  | spot | frame (hi-Z on) | vista off saves | hi-Z saves | hidden | hiz-build GPU | noise band |
  |---|---|---|---|---|---|---|
  | open-sea-low | 16.4-17.1 | 3.8 | -0.52 | 0 % | 1.45 | 2.07 |
  | village1-night | 21.6-21.8 | 6.0 | 3.34 | 51 % | 1.36 | 0.93 |
  | village1-hut-lamps | 23.0-23.1 | 6.9 | 3.64 | 93 % | 2.08 | 1.44 |

  The open-sea-low loss is inside its noise band; the two village1 savings are outside theirs.
- FLIP at 4K, hi-Z off against on, separate boots (`sandover-capture compare`), against a floor
  of two hi-Z-on boots: open-sea-low 0.047 (floor 0.040, differences on the water only, as in
  the paused A/B of r1), village1-night 0.011 (floor 0.016). village1-hut-lamps is not stable
  between boots: two hi-Z-off boots differ by 0.178 (framing changes; rig issue, not this change),
  so it rests on the samples-passed proof.
- Vanilla path: the reduction and the test run only inside the vista path (`vista` on, neighbour
  mode); with vista off nothing is allocated or drawn.

## 4. Not done, and why
- Draw merging per neighbour: the per-texture draws of a tree need array or bindless textures
  (2026-10-10 note). With hi-Z the hidden draws are gone; the visible ones are fragment-bound
  (60 %), where merging saves nothing. Not attempted tonight.
- LOD or distance cutoffs: no TIE or tfrag node projects under 4 px at 4K at these spots
  (2026-10-10), and the lower LODs drew slower. Nothing sub-pixel to cut.
- Skipping passes that contribute nothing: the alpha pass is under 0.1 ms; whole levels behind
  the depth are now skipped by the hi-Z level test (beach at the hut).
- Frustum tightness: the hi-Z test also rejects bounds off screen (the game's planes are wider),
  folded into the same counter; not split out.

## 5. Next
- The reduction pass costs 1.4-2.1 ms GPU at 4K whatever it culls, also after splitting it into
  two passes, which points at the depth read itself (a depth decompression on this GPU) rather
  than the arithmetic. Levers: reduce from the water depth copy when the mid ocean already made
  one, a compute reduction, or skip the build while the previous frame hid nothing; that last one keeps the output exact and
  only risks the saving. That would turn open-sea-low neutral and add
  about 1.5 ms at the village spots.
- The 32-bit perf-opt mask has no free bit (bit 29, the obvious candidate, gates the cached
  `perf_opt` uniform location), so the switch is vista option 15; widen the mask before the next
  optimisation needs one.
- Rig: village1-hut-lamps frames are not stable between boots (framing differs; FLIP 0.18 between
  two identical arms).
