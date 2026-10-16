# LOD and distant-geometry facts (Jak 1, PC renderer)

Pinned at `64d72d103`. Issue #19. Finding note: `findings/2026-09-23-lod.md`.
Paths are repo-relative. Status: verified = read in source; inferred = follows from code.

## 1. Tree kinds and LOD storage

| claim | status | source |
|---|---|---|
| tfrag tree kinds extracted: NORMAL, DIRT, ICE, LOWRES, TRANS, WATER (+ lowres-trans in the name table); nothing is dropped by kind | verified | `decompiler/level_extractor/extract_tfrag.cpp:2213-2229`; `common/custom_data/Tfrag3Data.h:334` |
| tfrag: every GOAL tree is extracted 3 times, one `TfragTree` per geometry LOD (`GEOM_MAX = 3`) | verified | `extract_tfrag.cpp:17,2210-2211` |
| tie: all 4 proto geometry LODs extracted (`GEOM_MAX = 4`), one hardcoded skip for a bad geo in one level | verified | `extract_tie.cpp:175,2753-2757`; `Tie3.h:143` ("includes 4 lods") |
| shrub: no LOD tiers in extractor or renderer; no `lod_shrub` setting | verified | `extract_shrub.cpp`; `game/graphics/gfx.h:83-84` |
| Sky dome: `SkyRenderer` replays GS packets through a `DirectRenderer` in bucket 3; sky texture time-of-day blend runs per level in `SkyBlendHandler` (buckets 32, 39) | verified | `SkyRenderer.h:13-51`; `SkyRenderer.cpp:11-19,133-172`; `OpenGLRenderer.cpp:758-778` |
| No cross-level far/backdrop tree: distant scenery is ordinary tfrag/tie (incl. LOWRES kind) of the loaded levels | inferred (absence of any stitching code) | `extract_tfrag.cpp`, `TFragment.cpp` |

## 2. LOD selection on PC

| claim | status | source |
|---|---|---|
| tfrag and tie LOD is one **global** index, not per-object distance: `lod()` returns `g_global_settings.lod_tfrag` / `lod_tie` | verified | `background/TFragment.h:77`; `background/Tie3.h:87` |
| Both default to 0 (highest detail) and are set from PC settings `lod-force-tfrag` / `lod-force-tie` (default 0); the PS2 distance path is disabled (`(when #t ;; (not (-> obj ps2-lod-dist?))`) | verified | `game/graphics/gfx.h:83-84`; `goal_src/jak1/pc/pckernel-h.gc:178-179,391-392`; `pckernel-common.gc:136-138`; `game/kernel/common/kmachine.cpp:1002-1012` |
| No distance fade or distance cull in `TFragment.cpp`, `Tie3.cpp`, `Shrub.cpp`, `background_common.cpp` | verified (grep) | same files |
| Only distance constants on the GOAL side: `*subdivide-settings*` 30 m / 70 m (PS2 near-tfrag subdivision), not used by the PC renderer | verified | `goal_src/jak1/engine/gfx/background/subdivide-h.gc:9-22`; `subdivide.gc:112-130` |
| `lowres` tfrag trees are submitted by GOAL as a separate list | verified | `goal_src/jak1/engine/gfx/tfrag/tfrag-methods.gc:643-656` |

## 3. Culling

| claim | status | source |
|---|---|---|
| Culling = 4 frustum planes (sphere test) AND the BSP visibility bitstring for the level, in one pass per tree per frame | verified | `background_common.cpp:489-505`; `TFragment.cpp:440` |
| The vis string is the GOAL-computed PVS copied per level into `render_state->occlusion_vis[level]` | verified | `TFragment.cpp:99-118`; `goal_src/jak1/engine/level/bsp.gc:158-180` |
| Planes, `itimes`, camera matrix and fog params arrive per bucket in `TfragPcPortData`, built from `*math-camera*` (one camera) and `(-> lev mood itimes)` | verified | `goal_src/jak1/engine/gfx/tfrag/tfrag.gc:262-282`; `background_common.h:7-24`; `TFragment.cpp:229-232` |
| Which vis set is authoritative depends on which level's boxes contain the camera | verified | `goal_src/jak1/engine/level/level.gc:1188-1312,738-757` |

## 4. Palette sharing (input to unbake)

| claim | status | source |
|---|---|---|
| tfrag palette = one packed time-of-day palette per GOAL tree; the 3 LOD copies each carry a copy of **the same** palette (all built from `tree->time_of_day`) | verified | `extract_tfrag.cpp:2181-2183,2211` |
| tfrag vertices hold a `u16 color_index`; dedup merges vertices only if position, UV and colour index all match | verified | `Tfrag3Data.h:100-111` |
| tie palette is **per proto**, shared by all instances and all 4 LODs; each instance supplies its own per-fragment colour *index lists* into that palette (`color_index_offset_in_big_palette` is the same for every instance of a proto) | verified | `extract_tie.cpp:2085-2129,663-666,186-193` |
| shrub palette = one per tree; each instance group carries a `color_index` | verified | `extract_shrub.cpp:537-541,604` |

Refinement of `facts/mood.md` claim 5: instances do not own palette *slices*; they own
index lists into the proto's shared slice. Two instances of a proto at different places can
reference the same palette entry.

## 5. Design implications

- **Unbake**: fit per palette entry, not per vertex. A tfrag entry is shared by all 3 LOD
  copies (same source palette), so an unbake solved on LOD 0 carries to the others if it is
  written back per entry. A tie entry can be shared by several instances in different
  positions and orientations, so a per-entry directional fit mixes their normals; the fit
  must either treat the entry as a multi-sample observation or split entries per instance
  (costs palette size, `ASSERT(result.colors.size() < UINT16_MAX)` at `extract_tie.cpp:2127`).
- **SSAO**: no LOD pops on PC by default (LOD fixed at 0), so AO does not see geometry
  switch with distance. Both levels' opaque geometry is in the single depth buffer by the
  end of bucket 30 (`facts/levels.md` §3).
- **Stereo**: culling uses one set of frustum planes and one vis string per level per frame.
  Per-eye rendering needs either the union frustum (cheapest: widen planes to cover both
  eyes) or a second cull pass. LOD needs nothing per eye while it stays global. Vis
  authority is decided by one camera position; with a 6-7 cm IPD both eyes fall in the same
  box set except at box edges (inferred).
- **User LOD override**: if `lod-force-*` is raised, all tfrag/tie in the frame switch
  together; an unbake written per palette entry stays valid (tfrag palettes are copies,
  tie palettes are proto-wide).
