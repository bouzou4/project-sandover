---
title: Can the engine draw a far, unloaded level's background in place? (phase 2 render hack)
date: 2026-09-23
issue: #22
status: verified (renders in place, cost, fog comparison) / inferred (production design)
sha: 6098ce9be + branch e11/vista-hack
---
## Answer (≤5 lines)
Yes. With a debug toggle, village2's tfrag, tie and shrub draw from village1's camera at their true
world position. They sit exactly where village1's low-res stand-ins of village2 stood. GOAL never
activates the level: the loader keeps it in its spare third slot. It costs about 0.9-1.1 ms of GPU
time for the whole of village2 with frustum-only culling (1440p, MSAA 4). Under village1's own fog
(end 1640 m) the vista is invisible. It needs far fog, and only fog pushed out for the whole frame
looks coherent.

## Evidence (path:line@sha / URL, short excerpts)
Code (branch `e11/vista-hack`):
- `game/graphics/opengl_renderer/VistaRenderer.{h,cpp}` owns one `TFragment` (kinds `{NORMAL}`),
  one `Tie3` and one `Shrub`. They are created lazily on first use and draw at the bucket-30 hook
  (`OpenGLRenderer.cpp`, `dispatch_buckets_jak1`), before the SSAO capture point.
- The camera is this frame's `TfragPcPortData`, taken from the level-0 tfrag renderer (level 1
  as fallback) through `TFragment::camera_this_frame`. Culling is frustum only
  (`occlusion_culling = nullptr`). Level 0's itimes drive the vista's palette.
- Residency: `gl_set_levels` (`pipelines/opengl.cpp`) appends the vista level to the loader's
  want list. The vista renderers call `get_tfrag3_level` every frame, which keeps it from being
  evicted. GOAL's level slots, actors and processes are untouched.
- Loader risk (inferred, not exercised): the third slot is the loader's slack for level
  transitions. With a vista in it, a transition briefly holds four levels (loads are not capped by
  `max_levels`), and a vista load in flight delays the next main-level load (one load at a time).
  If the player enters the vista level, it is deduplicated and the vista stops drawing. Turning
  the vista off drops it from the want list, and it is evicted after 180 idle frames.
- Own fog: `vista_fog_camera` (`background_common.cpp`) rescales the fog slope (`fog.x`),
  `perspective[2][3]`, the camera w column and `hvdf_off.w` by the same factor. The homogeneous
  clip position and depth are unchanged, so the vista is placed exactly as before, and only the fog
  end moves. Fog start stays the main level's value.
- Stand-in hiding: a comma-separated substring filter matched against tfrag texture
  `debug_name` (`TFragment::vista_hidden_draws`) and tie `proto_names` (`Tie3::vista_proto_flags`,
  reusing the Jak 2 proto-visibility multidraw path). It is applied only to the level 0/1
  renderers, and only while the vista actually drew the previous frame. The default filter is
  `v1-lores-vil2,vil1-medres-wood`.
- Toggles (default off, Jak 1 only; kernel symbols registered under the existing Jak 1 gate):
  - ImGui: Renderer Debug → Vista.
  - GOAL: `(pc-set-vista "village2")` turns it on and `"none"` turns it off.
    `(pc-set-vista-hide "a,b")` sets the stand-in filter. `(pc-set-vista-option which val)`
    takes which = 0 hide stand-ins, 1 fog mode (0 own, 1 main), 2 fog end in m, 3 shrub,
    4 GPU timer.
- Stereo (#27): the camera comes from the tfrag packet after `apply_stereo_eye`, so the vista
  follows each eye's camera and frustum with no extra code. The vista's tie wind is frozen, so the
  two eyes match. Not captured in stereo.

Pair choice: the original specification's candidate "village1 from beach-start" does not work, because beach-start
keeps village1 loaded as level 1 (`level-info.gc` beach-start `:lev1 'village1`). The village3
viewpoints load ogre (start) or snow itself (farside). village1-hut (village1 + beach loaded)
looking at village2 (about 1.9 km away) has true-position stand-ins to compare against, so that
pair was used.

Captures: `$SANDOVER_DATA/shots/e11/`, rig on slot 3, noon, 2560x1440, MSAA 4.
- `v1-*`: camera at (-150, 60, 150) m looking at (60, 90, -1600) m (ground level near the hut).
  From here village1's cliffs hide almost all of village2. Only the removed stand-in and a faint
  sliver change.
- `high-*`: camera at (-120, 450, 100) m looking at village2. This is the clear proof.
  `high-crops.png` crops the same region from four shots: off with main fog pushed (the
  stand-ins), vista with main fog pushed, vista with own fog, and vista with stand-ins not
  hidden.

| shot | what |
|---|---|
| `*-off` | vista off, stock fog |
| `*-own-fog` | vista on, own fog end 6000 m, main fog untouched, stand-ins hidden |
| `*-own-fog-standins` | same, stand-ins not hidden (doubles) |
| `*-main-fog` | vista on with the main level's fog (end 1640 m) |
| `*-main-fog-pushed` | vista on, main fog; `*village1-mood-fog-table*` fog end set to 6000 m for all 8 hours |
| `*-off-pushed` | vista off, main fog pushed (the stand-ins at far fog) |

What rendered (high view):
- village2's huts, cliffs and the tree canopy sit on the spot where the `v1-lores-vil2-*` stand-ins
  were, at the same scale. With stand-ins not hidden, both overlap and interleave at the same
  depth (`high-own-fog-standins.png`), which confirms the true-position fit from phase 1.
- The stand-in filter hides too much. Hiding `vil1-medres-wood` removes a patch of village1's
  distant ground that village2 does not cover, which leaves a black hole (clear colour) to the
  left of village2. Some village2 underside and edge geometry reads as black too; it is never
  seen in-game.
- `main-fog`: the vista is almost pure fog colour, as predicted.

Fog comparison:
- Own fog: the vista is crisp while village1's terrain around it is heavily fogged, so it reads
  as pasted on. Usable as a debug view, not as art.
- Main fog pushed to 6000 m: the vista and the surrounding terrain match. It changes the whole
  level's look (the sea and far terrain lose their haze), and in this view it also reveals the
  stand-in horizon skirts sharply.
- Verdict: a vista needs one atmospheric model for the whole frame (distance and height falloff
  applied to main and vista geometry alike), not a separate fog for the vista. This matches
  ADR-0014's "fog becomes atmospheric falloff".

Cost (GPU timestamps; `vista-draw` wraps only the vista draws, `buckets` is the whole dispatch,
240-frame averages, ms):

| view | buckets off | buckets vista (own fog) | vista-draw | notes |
|---|---|---|---|---|
| high | 0.89-0.96 | 1.83-1.89 | 1.00-1.10 (min 0.83) | |
| high, main fog pushed | 0.84-0.96 (off-pushed) | 1.68 | 0.90 | |
| hut (v1) | 1.65-1.94 | 5.6-6.2 | 2.2-3.4 (min 0.68) | other game instances were running on the GPU at the same time; treat as an upper bound |

CPU side: the per-frame cost is `cull_check_all_slow` plus multidraw list building over
village2's BVH, which is small next to the main level's. It was not timed separately. The one-time
`update_load` for the vista trees on the render thread did not reach the 5 ms "TFRAG setup" log
threshold.

Parity:
- With vista off, every added path is behind `vista_enable`, the stand-in hide additionally
  needs `vista_drawn`, and the vista renderers are never constructed. There is no GL call or
  allocation until first enable. `gl_set_levels` is unchanged when off.
- Pixel check (verified): a baseline build (master plus only the rig's uncommitted capture
  edits) and this branch, vista never enabled, same high camera, two shots each in one session:
  0 px differ between the builds for both shots (`par-e11-base-off*` vs `par-e11-off*`). The
  noise floor inside one session (shot 1 vs shot 2, which have different clocks) is 725 572 px
  (max 39), identical in both builds. `par-e11-off` is also byte-identical to `high-off`, which
  was taken in an earlier session.

## Implications for the design
- The data and renderer seams hold: a far level is one extra set of background renderers fed
  with the main camera. It needs no transforms, no GOAL changes and no per-level fix-ups.
- Fog is the real design question. It needs a frame-wide atmospheric model before vista is
  worth shipping.
- Stand-ins need an explicit per-object list, not name substrings (see Appendix B).
- Budget: about 1 ms per far level at LOD 0 with frustum-only culling. Several far levels need
  per-level LOD (tfrag LOD 1-2, tie LOD 1-3 exist in fr3) and a coarse visibility test.

## Follow-ups
- Frame-wide atmospheric fog prototype (distance plus height), applied to all background
  renderers, then re-evaluate with the vista.
- Per-object stand-in manifest (level, kind, name, depicted level) generated from
  `standins.csv`, reviewed by hand, used instead of the substring filter.
- Per-renderer LOD override for vista draws; measure the cost of LOD 1-2.
- More than one vista level (loader `max_levels` > 3; memory from `facts/vista.md` §3).
- Capture in stereo mode to confirm the per-eye vista visually.

## Appendix B: status of the phase-2 hack
**What rendered.** village2 tfrag (NORMAL trees), tie (NORMAL and NORMAL_ENVMAP categories,
wind frozen) and shrub, from village1-hut's level-0 camera packet, in place. Not drawn: tfrag
trans/dirt/ice trees and tie alpha categories (alpha buckets), water, actors and merc. The
vista's own LOWRES tfrag trees are skipped too (they are that level's stand-ins of others).

**Fog.** Own fog (end 6000 m, main start, current fog colour) makes the vista visible but
mismatched. Main fog at stock settings hides the vista entirely. Main fog pushed (mood-table
patch over the REPL) is coherent but changes the level's look. Recommended: a frame-wide
atmospheric model.

**Cost.** Vista draw about 0.9-1.1 ms GPU for village2 at 1440p MSAA 4 from a high view. Dispatch
rises by about 0.8-0.9 ms. The hut numbers (2-3 ms) were taken while other instances were
running and are an upper bound.

**Stand-in handling.** The substring filter over tfrag texture names and tie proto names works
mechanically (the vil2 walls and canvas vanish when the vista draws). It is too coarse:
`vil1-medres-wood` also covers ground, which leaves a hole. A general mechanism needs:
- A stand-in manifest per viewer level: object id (tfrag draw or tie proto), the level it
  depicts, and a "covered by real geometry" flag. Seed it from the phase-1 fit
  (`median_dist_to_real_m`) and review it by hand.
- At runtime, hide a manifest entry only while the level it depicts is drawn as a vista (or
  loaded), keyed by level name and not by filter.
- Displaced stand-ins (the village1 snow proxy ≈1 km off) are always hidden when snow is a
  vista. Horizon skirts with no real counterpart stay.
- Prefer tagging at extraction time (a `standin_of` field per draw/proto in fr3) over string
  matching at runtime.

**What a production version needs.**
1. Loader: more than one spare slot (`max_levels`), a residency policy (distance- and
   visibility-based), and memory accounting (≈1-1.5 GiB for all backgrounds, `facts/vista.md` §3).
2. Culling: a coarse far-level visibility test (level bsphere against the frustum, plus a
   horizon/occluder test). Vis strings do not apply across levels.
3. LOD: per-renderer-instance LOD selection for far levels.
4. Fog: a frame-wide atmospheric model, instead of per-level mood fog being the only depth cue.
5. Palette: the viewer's itimes applied to the far level's colours is an approximation; unbake
   (unbake) or relighting replaces it.
6. Alpha: far-level trans/water buckets, drawn in the alpha phase after the main levels'.
7. Stand-ins: the manifest mechanism above.
8. Seams: depth precision at 2-5 km with a 0.25 m near plane was fine here (no visible
   z-fighting except where stand-ins and real geometry coincide). Re-check at 5-10 km.

**Parity.** Vista off takes no new GL path by construction, and in a pixel check it matches a
baseline build exactly (0 px, see Evidence).
