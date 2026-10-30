# Vista facts (Jak 1): world space, distance tricks, budget, renderer seams

Engine claims pinned at `5b7218d5f`; C++ lines read from the commit
(`git show 5b7218d5f:<path>`), not the working tree. Finding note:
`findings/2026-09-23-vista-facts.md`. Data: `$SANDOVER_DATA/vista/` (`levels.json`,
`levels.csv`, `level_info.json`, `adjacency.csv`, `continues.csv`, `standins.csv`,
`points/`), produced by `sandover/tools/sandover/vista/` (`glb_stats`, `level_info`,
`adjacency`) from the unbake rip of `<level>-background.glb`.

Status legend: **verified** = read in source at the pinned sha, or measured directly on the
rip; **inferred** = follows from verified facts or from a geometric fit, not observed at runtime.

## 1. World-space consistency

| claim | status | source |
|---|---|---|
| The glb exporter writes LOD 0 only (tfrag tree set 0, tie tree set 0, all shrubs) and divides positions by 4096, so glb units are metres | verified | `decompiler/level_extractor/fr3_to_gltf.cpp:199-203,1814-1823@5b7218d5f` |
| All levels share one continuous world coordinate space at one scale: 17 of 19 land-adjacent pairs have coincident vertices (closest approach 0.0 m on a 0.25 m grid, 22-212 touching points per pair) | verified (measured) | `adjacency.csv`; pairs from `level-info.gc` continue points and `load-boundary-data.gc` `(load a b)` |
| Remaining land pairs: firecanyon/village1 0.25 m (33 touching points), maincave/village3 0.35 m (2), citadel/finalboss 0.94 m (0; joined by an elevator) | verified (measured) | `adjacency.csv` |
| Sea gaps, by design (islands): training/village1 978 m, village1/misty 488 m closest approach | verified (measured) | `adjacency.csv` |
| Adjacent levels' boxes overlap by up to 323 m (village3/snow, smallest-axis depth); sunkenb sits inside sunken (308 m) | verified (measured) | `adjacency.csv` (box = geometry without stand-ins, `detail_bbox_m`) |
| Every gameplay continue point lies inside its own level's geometry box (0 m), except `beach-start` (31.5 m outside, on the village1 side). `:bsphere` values are in the same units | verified (measured) | `continues.csv`; `goal_src/jak1/engine/level/level-info.gc@5b7218d5f` |
| Some tie position buffers hold unreferenced vertices up to ±10 km away; bounds must be taken over indexed vertices only | verified (measured) | village1 tie: whole buffer spans ±10.8 km, indexed vertices do not |

## 2. Fog, far plane and distant geometry

Fog tables (`goal_src/jak1/engine/gfx/mood/mood-tables.gc@5b7218d5f`; `mood-fog` layout
`mood-h.gc:12-19`: `fog-dists` x = start, y = end, z/w = max/min intensity). Start/end in m.

| level(s) | table line | start / end (m) | varies by hour |
|---|---|---|---|
| village1, beach, jungle | 481 | 64 / 1640 (hours 6, 7: 1217, 901) | colour and end |
| training | 732 | 64 / 2500 | colour |
| snow | 805 | 200 / 650 | colour |
| jungleb | 1068 | 24 / 750 | no (one entry) |
| maincave | 1142 | 106 / 256 | no |
| robocave | 1216 | 64 / 500 | no |
| darkcave | 1240 | 64 / 250 | no |
| misty | 1314 | 0 / 180 | colour |
| village2 | 1487 | 64 / 450 | colour |
| swamp | 1676 | 5 / 191 | colour |
| sunken, sunkenb | 1865 | 64 / 330 | two entries |
| rolling | 1966 | 64 / 546 | colour |
| firecanyon | 2155 | 32 / 500 | colour |
| ogre | 2363 (ogre3 context: 2626, 96 / 1821) | 64 / 546 | colour |
| village3 | 2680 | 64 / 546 | colour |
| lavatube | 2814 | 64 / 452 | no |
| finalboss | 3010 | 64 / 5500 (entry 3: 222 / 467) | colour |
| citadel | 3142 | 64 / 550 | no |

Level-to-table mapping: `mood-tables.gc:3216-3614`. Status: verified.

| claim | status | source |
|---|---|---|
| The blended mood fog is written once per frame into `*math-camera*` fog-start/end/max/min | verified | `goal_src/jak1/engine/gfx/mood/time-of-day.gc:398-401@5b7218d5f` |
| Camera near plane 0.25 m (`d` = 1024), far plane 10 000 m (`f` = 40 960 000); no PC override of `f` found | verified | `goal_src/jak1/engine/gfx/math-camera.gc:238-239@5b7218d5f` |
| On PC, LOWRES tfrag trees are drawn in the normal tfrag bucket (the kind list is `{NORMAL, LOWRES}`); GOAL submits them per level | verified | `game/graphics/opengl_renderer/OpenGLRenderer.cpp:658-659,684-685@5b7218d5f`; `goal_src/jak1/engine/gfx/background/background.gc:301-313@5b7218d5f` |
| Low-res stand-ins exist in training, village1, misty, village3 and finalboss (tfrag-lowres trees plus `*-lores-*` tie protos); 69 objects in total | verified (measured) | `standins.csv` |
| Most stand-ins sit at the **true position and true scale** of the neighbour they depict: rolling precursor towers (median 0.4-4 m from the real rolling geometry), village2 walls and canvas (4-8 m), finalboss's snow pieces (7-10 m) | inferred (vertex-distance fit) | `standins.csv` `median_dist_to_real_m` |
| One **displaced** stand-in: village1's `v1-lores-snow` + `vil-lores-vil3-wall` (also in training and misty) is Snowy Mountain moved ≈1070 m west (−x) and 70 m in z at scale ≈0.9-1.0 (median residual 5.5 m over 218 samples). Same distance from the village (≈3.6 km), bearing shifted ≈17° west | inferred (translation+scale fit) | `standins.csv`; fit against `points/snow.npy` |
| Horizon skirts with no real counterpart anywhere: `v1-lores-mountains` (14.7 km wide), `vil1-medres-grass/rock/beach` (3-14 km), `vil3-lores-treetop`, `bigpuff`; median 0.3-5.7 km from any level's geometry | inferred | `standins.csv` |
| Swamp blimp stand-ins (village1, misty, training, village3, finalboss) have no background counterpart (≈150 m from swamp geometry); they likely depict an actor | inferred | `standins.csv` |
| Stand-ins are drawn fogged: seen from village1 (fog end 1640 m) the rolling towers (≈1.6 km) sit at the fog limit and the snow proxy (≈3.6 km) is past it | inferred | fog table above; stand-in positions |

## 3. Budget (LOD 0, from the rip; 21 gameplay levels)

| level | tris | tfrag | tie | shrub | verts | tex MiB | est. VRAM MiB | fr3 MiB |
|---|---|---|---|---|---|---|---|---|
| training | 393 864 | 30 028 | 313 468 | 50 368 | 532 854 | 2.7 | 24.4 | 24.8 |
| village1 | 570 866 | 64 404 | 405 043 | 101 419 | 736 355 | 4.2 | 34.7 | 35.0 |
| beach | 430 884 | 94 002 | 274 002 | 62 880 | 548 224 | 4.1 | 27.1 | 37.4 |
| jungle | 531 778 | 55 146 | 306 482 | 170 150 | 708 052 | 3.7 | 32.7 | 39.8 |
| jungleb | 82 640 | 76 | 82 204 | 360 | 192 434 | 1.6 | 9.0 | 10.2 |
| misty | 666 197 | 60 853 | 513 882 | 91 462 | 883 685 | 3.1 | 38.7 | 42.0 |
| firecanyon | 614 456 | 16 990 | 525 914 | 71 552 | 817 914 | 3.1 | 36.2 | 23.0 |
| village2 | 577 320 | 37 864 | 325 227 | 214 229 | 759 371 | 4.9 | 36.3 | 36.9 |
| sunken | 460 908 | 64 512 | 318 760 | 77 636 | 820 564 | 2.5 | 33.6 | 32.7 |
| sunkenb | 215 332 | 22 784 | 121 524 | 71 024 | 340 238 | 2.5 | 16.2 | 16.8 |
| swamp | 735 179 | 27 299 | 480 450 | 227 430 | 969 061 | 3.4 | 42.5 | 31.5 |
| rolling | 782 138 | 54 664 | 474 890 | 252 584 | 1 311 292 | 2.8 | 52.7 | 37.7 |
| ogre | 969 886 | 36 072 | 643 082 | 290 732 | 1 375 658 | 3.6 | 57.8 | 43.7 |
| village3 | 367 434 | 123 751 | 153 625 | 90 058 | 508 731 | 3.7 | 24.6 | 28.3 |
| snow | 656 748 | 43 418 | 534 510 | 78 820 | 865 598 | 3.9 | 39.1 | 36.7 |
| maincave | 399 290 | 228 192 | 11 936 | 159 162 | 525 282 | 2.8 | 24.3 | 26.6 |
| darkcave | 203 882 | 143 138 | 46 008 | 14 736 | 259 930 | 2.1 | 13.1 | 20.3 |
| robocave | 462 542 | 182 436 | 156 954 | 123 152 | 610 014 | 2.9 | 27.8 | 31.6 |
| lavatube | 1 120 616 | 9 818 | 1 049 448 | 61 350 | 1 422 166 | 2.5 | 59.5 | 29.4 |
| citadel | 542 800 | 2 186 | 496 570 | 44 044 | 832 704 | 3.9 | 36.8 | 62.2 |
| finalboss | 223 594 | 42 906 | 127 088 | 53 600 | 341 192 | 1.1 | 14.5 | 19.0 |
| **total** | **11 008 354** | 1 340 539 | 7 361 067 | 2 306 748 | 15 361 319 | 65.1 | **≈680** | 666 |

- tris/verts: verified (counted on the rip). tie counts are per placed instance (the exporter
  unpacks instances into world space).
- tex MiB: RGBA8 of the textures the background glb references (envmap-strength copies
  excluded). The texture census has no tpage-to-level map, so per-level texture memory comes from
  the glb instead. Counted per level, so shared textures count once per level. Verified.
- est. VRAM = verts × 32 B + tris × 3 × 4 B + textures × 4/3 (mips). Inferred; a lower bound,
  because the loader keeps the other LODs, merc/actor data and the palettes too. The
  uncompressed fr3 size (first 8 bytes of each fr3) is the whole level in memory: 666 MiB
  over 21 levels. Estimate for all backgrounds resident: **≈0.7 GiB at LOD 0; plan for ≈1-1.5
  GiB with every LOD and fr3 overheads.**

## 4. Renderer seams (for phase 2)

| claim | status | source (`@5b7218d5f`) |
|---|---|---|
| `Loader` is created with `max_levels` = `jak1::LEVEL_TOTAL` = 3 (2 heap slots + 1) | verified | `common/goal_constants.h:45-47`; `game/graphics/pipelines/opengl.cpp:50-53,89-91` |
| GOAL's `__pc-set-levels` passes exactly two names; `gl_set_levels` replaces the loader's wanted list with them every call | verified | `game/kernel/jak1/kmachine.cpp:522-538`; `game/graphics/pipelines/opengl.cpp:833-834`; `loader/Loader.cpp:59-83` |
| The loader unloads only when full, choosing a level idle > 180 frames, not wanted first; `get_tfrag3_level` resets the idle counter | verified | `loader/Loader.cpp:36-45,414-429,534-560` |
| Buckets are split per heap slot (level0 5-11 / 19-24, level1 12-18 / 25-30); each TFragment/Tie3 gets a `level_id` 0/1; Shrub has none | verified | `buckets.h:6-78`; `OpenGLRenderer.cpp:684-744` |
| Each renderer learns its level from the `level_name` in the `TfragPcPortData` DMA packet and fetches it with `get_tfrag3_level`; TFragment then calls `setup_for_level` + `render_matching_trees(lod(), kinds, settings)` with `settings.camera` = the cached packet | verified | `background/TFragment.cpp:129-163,231,360-372`; `Tie3.cpp:222`; `Shrub.cpp:175` |
| `add-pc-tfrag3-data` sends: 4 frustum planes, 4 itimes (mood palette weights), camera matrix, hvdf offset, fog (pfog0, fog-min, fog-max), camera position, rotation, perspective, level name | verified | `goal_src/jak1/engine/gfx/tfrag/tfrag.gc:262-297`; `background/background_common.h:7-24` |
| Culling is frustum + vis bits; with a null vis string it falls back to frustum only. Vis is copied for 2 levels only (`num_vis_to_copy = LEVEL_MAX`) | verified | `background/background_common.cpp:489-505`; `TFragment.cpp:99-118`; `OpenGLRenderer.cpp:1305-1306` |
| Fog distances are uniforms per draw from the packet; fog colour is one global per frame | verified | `background_common.cpp:231-276` (uniforms :271-272); `OpenGLRenderer.cpp:1320` |
| Precedent for drawing without new DMA: `Tie3AnotherCategory` and `Tie3WithEnvmapJak1` call `render_from_another` with the settings cached from the parent's last packet | verified | `background/Tie3.cpp:1013-1036` |
| Hook precedent between opaque and alpha: the collision debug draw at bucket 30 | verified | `OpenGLRenderer.cpp:1359` |

**Smallest phase-2 change (inferred):** one extra `TFragment` (kinds `{NORMAL}`) and one
`Tie3` owned by `OpenGLRenderer`, not bound to a bucket. In `dispatch_buckets_jak1`, at the
bucket-30 hook, call `setup_for_level(kinds, "<vista>", ...)` and
`render_matching_trees(lod(), kinds, settings, ...)` (tie: the equivalent draw call) with
`settings.camera` copied from the level-0 TFragment's cached `m_pc_port_data.camera` (needs
an accessor) and `occlusion_culling = nullptr`. Keep the level resident by appending it in
`gl_set_levels` and `set_active_levels`; with 2 gameplay levels + 1 vista this fits
`LEVEL_TOTAL` = 3 as it stands. Caveats: level0's itimes drive the vista palette; fog is the
viewer's fog (so a 450-1640 m fog end hides the vista unless fog is pushed out); the
viewer's own low-res stand-ins of the same place must be hidden to avoid doubles; no vis
means every node is frustum-tested.
