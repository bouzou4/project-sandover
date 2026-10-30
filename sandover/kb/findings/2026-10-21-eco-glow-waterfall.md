---
title: Why is the blue eco glow cut by a hard edge where it meets the village2 waterfall?
date: 2026-10-21
issue: none (player bug report 20261021-202031)
status: proven (rig before/after on the bundle)
sha: 7cc714ec9 (base; branch fix/eco-glow-waterfall); report build ec4a22c8f
---
## Answer (5 lines)
Draw order, not depth. The waterfall mist (falls-particle sprites, about 70 m away, alpha
blended) is drawn after the blue eco cluster's additive sprites (26 m) in the same sprite
flush, because Sprite3 batches world sprites by texture and draw mode and draws the batches in
first-seen order. Where the mist is visible it paints over the eco; where the near gorge rock
hides the mist (depth test), the eco stays. The cut is the rock's silhouette.

## The report
`$SANDOVER_DATA/runs/20261021-2003-play/bug-reports/sandover-bug-20261021-202031.zip`:
village2, continue `village2-start`, hour 12.2, camera (212.5, 15.4, -1614.3) m, FSR 3.1 at
67 % (internal 2560x1440), HDR10 PQ, FP16 target, bloom on. The player calls it an eco vent and
suggests "waterfall particles z-fighting".

The emitter is a blue eco cluster, not a vent: `eco-27` (`eco-info 3 1`) at
(208.2, 11.5, -1639.7), 26.1 m from the camera (`decompiler_out/jak1/entities/village2-actors.json`,
light census row `eco`, colour 0.42, 0.47, 1.0). The waterfall is particles only: the launcher
`villageb-part-32` (`group-village2-waterfall-32`) sits at (196.0, -0.9, -1680.0), 69.7 m away,
and village2 has no waterfall texture in any tpage (`village2-vis-{tfrag,alpha,water,shrub,pris}`;
the only water textures are `ya-water`, a pond at x 364-424, and `cv-water`, the bucket).

## The frame (4K screenshot)
- The edge is one output pixel wide. Across the horizontal step at x 1790: y 1035 (255,255,255),
  y 1036 (125,175,255), y 1037 (80,129,251). Across the vertical step at y 1110: x 1777
  (74,140,255), x 1778 (65,125,215).
- It is not a tile edge. At the internal scale the step sits at about (1185, 690); 690 is on no
  8/16/32/64 px grid, and the same boundary continues above the eco as diagonal and vertical
  segments up the whole left side of the waterfall (crop x 1640-2340, y 380-1200): the outline of
  the dark gorge wall in front of the falls. The glow probe grid (`glow_*` shaders) is Jak 2/3
  only (`Sprite3::render_jak2` calls `glow_dma_and_draw`; `render_jak1` does not).
- The eco is under the mist, not rejected: a high-pass crop shows the bolts continuing faintly
  into the mist past the edge. A depth-rejected sprite would leave nothing.
- Bloom crosses the edge: right of the step the blue falls off over 12-60 px (y 1080: b 254 at
  x 1781, 213 at 1784, 181 at 1790), the bloom of an emissive mask that blended sprites never
  clear (`emissive_draw_buffers(false)` for them). Only the sprite colour is cut.

## Mechanism (code)
- Particles never write depth: `particle-setup-adgif` sets the zbuf register with `zmsk 1`
  (`goal_src/jak1/engine/gfx/sprite/sparticle/sparticle-launcher.gc:234`).
- Blend: the launcher picks alpha register 0x44 (blend) unless the part has flag `bit3`, which
  selects 0x48 (additive) (`game/mips2c/jak1_functions/sparticle_launcher.cpp:573-576`). The eco
  cluster parts carry `bit3` (`collectables-part.gc`, `(:flags (bit2 bit3 bit14))`); the
  falls parts do not (`village2-part2.gc:1108`, `(:flags (bit2 bit12 bit14))`).
- Both are 2D-system parts (only part 1257 is `is-3d`), so they land in one world flush.
- `Sprite3::do_block_common` keys every sprite by `tbp << 32 | mode` and appends a new key to
  `m_bucket_list` the first time it appears (`Sprite3.cpp:1025-1034`); `flush_sprites` draws
  bucket after bucket. All falls sprites draw together, after every eco sprite when the first
  falls sprite in the stream follows the first eco sprite. That order comes from particle slot
  allocation, so it can flip as particles recycle.
- This batching is upstream OpenGOAL, so vanilla has the same order. The cut was faint there:
  the vanilla eco stack clips at 1. With the FP16 target and bloom the stack runs far above 1
  and the cut reads as a hard edge.

Ruled out: z-fighting (sprites write no depth and nothing is coplanar); a depth-writing sheet
(no waterfall geometry; bolts visible under the mist); FSR (the reactive mask and the
composition copy cannot remove colour that is in the current frame, and the boundary follows
scene geometry); emissive tile grid or glow probe grid (not on a grid, Jak 2/3 only).

## Fix (branch fix/eco-glow-waterfall)
On the Sandover render path (vanilla rendering off), Jak 1 world sprite flushes draw back to
front; the vanilla path keeps upstream's batch order, so parity is unchanged. The gate is a
property of the render path, not of any effect row: GOAL sends `pc-set-sandover-path` from
`sandover-settings-update` when `sandover-vanilla?` changes (engine default off, so a seeded
vanilla config never sorts); `Gfx::g_global_settings.sandover_path` gates the sort.

`Sprite3::sort_world_sprites` orders the sprites of the flush by the view w of their centre
(the shader's `matrix_transform`), ties in stream order. Runs then merge: a stretch of
consecutive additive sprites is drawn grouped by key (addition commutes), except listed light
textures (`emissive_table`), which MAX into the emissive buffer and so keep their depth order;
any other sprite joins the previous run only if it has the same key. Runs are reused across
flushes. `sandover-settings-reapply` forgets the sent path like every other setting. HUD flushes are untouched. Debug window:
"World back to front (Sandover path)" toggle and the draw count.

Why per sprite, not per batch: sorting the batches (for example by their farthest sprite) would
fix this frame, where the mist batch is all at 70 m and the eco batch at 26 m, but a batch
spans every depth its texture appears at (smoke, mist, embers in several effects at once), so a
batch order cannot be right for every pair. Cost class: one O(n log n) CPU sort of the world
sprites per flush (hundreds to low thousands per frame, a 12-byte key each), no extra GPU passes;
draw calls grow only where blended sprites of different textures interleave in depth. At the
report's spot: +0.17 ms render thread, +0.14 ms sprite GPU (Live proof).

## Live proof
Symptom: the eco glow is cut along the gorge wall's silhouette where the falls mist is in
view. Mechanism: the mist batch draws after the eco batch (first-seen order) and paints over it.
Proof: one build of this branch, the bundle re-shot with `from-snapshot` (player settings, 4K,
FSR 3.1 at 67 %), the order forced with `(pc-set-sandover-path 0|1)` after load. Edge metric over
x 1650-1999, y 900-1199: 1-px drops in blue > 25 inside the glow (b > 150, b - r > 40), and the
longest vertical run of them in one column (the cut is a vertical line).

| Frame | Sharp blue drops | Longest vertical run | Eco pixels (b - r > 100, b > 200) |
|---|---|---|---|
| Player's report | 157 | 25 px at x 1777 | 5786 |
| Path 0 (upstream order) | 224 | 19 px at x 1777 | 7135 |
| Path 1 (sorted) | 35 | 8 px at x 1804 | 14597 |
| Path 1, second run | 56 | 6 px at x 1716 | 14291 |

The upstream-order frame reproduces the cut in the report's column; both sorted runs have none
and the glow continues over the mist (twice the eco pixels). The cluster's bolts move between
runs (particle phase and temporal AA are not frame-identical: two runs of one setting differ in
5-10 % of pixels by more than 8), so the measure is the edge, not a whole-frame diff.

Unrelated sprite spots (`eco-emitters.yaml` `citadel-torch`, `jungle-vent-night`), vanilla look
with the sort forced on against two runs with it off: sorted vs off differs in 9.4 and 9.6 % of
pixels, the two off runs in 9.7 and 8.6 %; nothing visible. Vanilla parity: with vanilla rendering
on, GOAL sends `pc-set-sandover-path 0` (`*sandover-path-sent*` read back 0), so the engine keeps
upstream's order; with the Sandover look it reads back 1.

Cost, same spot and frame, path 0 vs 1, 30-frame windows (not solo: another rig instance shared
the GPU): render thread 13.08 vs 13.25 ms (+0.17 ms), sprite GPU 0.08 vs 0.22 ms (+0.14 ms).

Frames and logs: `$SANDOVER_DATA/shots/eco-glow-427-proof/` (`eco-off`, `eco-on`, `probe`,
`van-*`, `torch-*`, `logs/`).

Rig notes from this run: the first probe lost the `rig-set-camera-row 0` send (an nREPL
connection error in goalc's log) and shot a mirrored view with camera check ok; re-run, it
landed. At the torch and vent spots the play look drew no tfrag/tie (`glDrawBuffers` errors on
the tfrag buckets) with the path at 0 and 1 alike; the vanilla look drew them.
