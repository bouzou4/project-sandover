---
title: Can far, unloaded levels be drawn in place as a vista? (phase 1 facts)
date: 2026-09-23
issue: #22
status: verified (measurements, source) / inferred (stand-in fits, phase-2 change)
sha: 5b7218d5f
---
## Answer (≤5 lines)
Yes on the data side. Jak 1 is one continuous metre-scale world: 17 of 19 land-adjacent pairs
share coincident vertices, the worst land seam is 0.94 m (citadel/finalboss, an elevator),
and the only big gaps are the sea around the islands (978 m, 488 m). The original game already
fakes distance with low-res stand-ins, mostly at true position and scale, plus one displaced
Snowy Mountain and km-wide horizon skirts. All 21 backgrounds: 11.0 M tris, ≈0.7 GiB at LOD 0.

## Evidence (path:line@sha / URL, short excerpts)
- Data (verified, measured): `$SANDOVER_DATA/vista/adjacency.csv` (box relation + KD-tree
  closest approach on 0.25 m-quantised vertices), `continues.csv`, `standins.csv`,
  `levels.json`/`levels.csv`; tools `sandover/tools/sandover/vista/{glb_stats,level_info,adjacency}.py`.
- Exporter writes LOD 0 in metres: `decompiler/level_extractor/fr3_to_gltf.cpp:199-203,1814-1823@5b7218d5f`.
- Pairs: `goal_src/jak1/engine/level/level-info.gc` continue points (`:lev0`/`:lev1`),
  `load-boundary-data.gc` `(load a b)` (21 pairs incl. title/village1, which is a menu).
- Fog: `goal_src/jak1/engine/gfx/mood/mood-tables.gc:481` village1 `:fog-dists :x 262144.0 :y 6717440.0`
  (64 m / 1640 m); `time-of-day.gc:398-401` writes it to `*math-camera*`; far plane
  `math-camera.gc:239` `(set! (-> gp-0 f) 40960000.0)` = 10 km.
- LOWRES tfrag drawn on PC: `game/graphics/opengl_renderer/OpenGLRenderer.cpp:658-659@5b7218d5f`
  `normal_tfrags = {NORMAL, LOWRES}`. Closes the level catalogue LOD follow-up.
- Displaced stand-in (inferred): village1 `v1-lores-snow` + `vil-lores-vil3-wall` fitted to
  `points/snow.npy`: best shift (+1070, +20, +70) m, scale 0.9, median residual 5.5 m, p75 16.5 m.
- Renderer seams: `loader/Loader.cpp:36-45,59-83,414-429,534-560`; `pipelines/opengl.cpp:89-91,833-834`;
  `kernel/jak1/kmachine.cpp:522-538`; `background/TFragment.cpp:99-118,129-163,360-372`;
  `background_common.cpp:489-505`; `Tie3.cpp:1013-1036`; `OpenGLRenderer.cpp:1305-1306,1320,1359`
  (all `@5b7218d5f`). Full table in `facts/vista.md` §4.

## Implications for the design
- Vista can use each level's own fr3 unmodified: positions are already in world space, so
  no per-level transform is needed (verified).
- Smallest phase-2 change (inferred): an extra `TFragment` + `Tie3` owned by `OpenGLRenderer`,
  drawn at the bucket-30 hook with level0's cached camera packet, `occlusion_culling = nullptr`,
  and the vista level kept resident via `gl_set_levels`/`set_active_levels`. One vista level
  fits the existing `LEVEL_TOTAL` = 3 loader slots; more need a larger `max_levels`.
- Fog decides visibility: in-game fog ends at 180-650 m in most levels (1640 m in village1,
  2500 m training, 5500 m finalboss), so a vista beyond that renders as fog colour. The
  feature needs a fog policy (longer fog end, or vista-only fog) — an art decision.
- Doubles: where a stand-in depicts the vista level (village1 → rolling, village2, snow),
  the stand-in must be hidden when the real level is drawn; the displaced snow proxy would
  otherwise show two Snowy Mountains ≈1 km apart.
- Palette: level0's itimes applied to the vista level's 8 colour sets is an approximation;
  unbake/relight work would replace it anyway.
- Cost: 11 M tris if everything is drawn at LOD 0; the fr3 already carries tfrag LOD 1-2 and
  tie LOD 1-3, which the global `lod_tfrag`/`lod_tie` switch cannot pick per level today.

## Follow-ups
- Phase 2 (separate issue): the render hack, one vista level from village1 (rolling or
  village2 are the best test: true-position stand-ins exist for an A/B).
- Confirm on a capture that the rolling/village2 stand-ins are visible from village1 at
  day fog (inferred at the fog limit).
- Per-level LOD selection for vista draws (needs a `lod()` override per renderer instance).
- Identify what the swamp blimp stand-in depicts (likely an actor, not background).
