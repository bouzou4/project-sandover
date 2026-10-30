---
title: Glow prop crystals keep their light under the world sun map
date: 2026-10-28
issue: Mountain Pass crystals lavender by day (finding 2026-10-28-mountain-pass-crystal-lights, colour section)
status: verified (rig, three Mountain Pass cameras, snow, two cave cameras)
sha: 627939c5c (base)
---
## Answer (5 lines)
1. By day the world sun map reads a glow prop's bright baked colour as sun. The unbake fit has no
   way to tell a texture painted bright on purpose from a sunlit one, so most of the crystal's
   colour is fitted as "direct sun share". The map then finds the crystal in shadow (it sits in a
   rock hollow, or its own blades cover it) and removes that share; the sky hue tints what is left
   toward the ambient's blue. Pink crystals came out lavender, blue snow crystals dark blue.
2. Fix: a texture the light bake lights as a glow prop on a level is self-lit. While the world
   map is on, its receivers take no removed share, no sky hue and no canopy (`ws_receive`, 0 on
   a glow prop draw, scales those three terms only); the PBR sun terms (glints, parallax shadow,
   wet-film lobe) keep the real occlusion.
3. Source of truth: the bake's own prop decision. `sandover-lights bake` writes
   `SelfLitTextures.inc` ({level, texture} for every prop texture that got glow rows) in the same
   run as `WorldLights.inc`, so a texture is self-lit exactly when it gives light.

## Why the bake list, not day E in the sidecar
Day E would need a second decision (which textures emit by day) written into the unbake
sidecars, which the prop pass already makes from vanilla frames (`props.glows_in_vanilla`).
Two places would drift; the cave crystals have no E at all, only the prop row. The list is
generated, tested against the census (`test_selflit_inc_is_current`) and keyed by texture name,
which the loader resolves to the level's texture indices once at load (`tex_selflit`), like the
sand list.

## What the GPU does now
- Loader: `world_lights::self_lit_textures(level, names)` marks the level's textures.
- Tie3 (normal and envmap base draws), TFragment and Shrub set the per-draw uniform
  `ws_selflit` when the draw's texture changes (reset to 0 at each tree).
- Fragment (tfrag3, etie_base, shrub): `rcv = ws_receive(shadow_receive)` is 0 when the world
  map is on (`ws_on & 1`) and `ws_selflit` is 1. `rcv` scales the removed share, the sky hue and
  the canopy; `pbr_sun_occ` still comes from `shadow_receive` and the real occlusion, so a glint,
  the parallax shadow raise or the wet-film sun lobe on a shadowed glow prop stays shadowed. On a
  self-lit pixel the map lookup runs only when one of those PBR terms reads it
  (`pbr_sun_occ_read`). The alternative, a PBR sun share of 0 on self-lit draws, would also drop
  those terms on sunlit faces, a look change; this keeps them as master computes them. Today it
  changes no pixel: the listed textures are class special (unshaded) or foliage in sky-less
  caves, and only class snow has glints. Option 32 off is the master path exactly (no GL call for
  `ws_selflit`); wind ties (foliage) are untouched.
- The envmap second pass needed more. The Mountain Pass crystals are envmapped ties: a base
  draw (`etie_base`) and an additive reflection draw (`etie`). The extractor keys the second-pass
  draws by the envmap texture and gives every second-pass strip its own copy of the vertices,
  so neither the draw's texture nor the base draw's vertex ids identify the crystal there. With
  only the per-draw flag the base was fixed but the reflection was still darkened and tinted by
  the map (crystal 216, 116, 115 against 231, 137, 129 with the map off). Tie3 builds, at level
  load while option 32 is on (or once when it is turned on later), a per-vertex mask: a second-pass vertex is self-lit where a self-lit
  base draw has a vertex at the same position (exact float match; ogre tree 0: 8560 second-pass
  vertices from 3392 base positions; maincave and robocave 3900 each; ogre with the crystal row: 34240
  vertices over the 4 LOD trees in 1.4 ms, one log line per level). The mask is an R8UI texture
  buffer on unit 58 (sampler bound at link), bound and `ws_selflit` = 2 set only while the map is
  on; `etie.vert` reads it at `gl_VertexID`. The etie pass has no PBR terms, so there the vertex
  zeroes `shadow_receive` itself.
- Today's list: `cv-bluecrystal`, `cv-mushroom-01-decal-01`, `cv-tallshroom-decal-01` (maincave,
  robocave: no world map there since the cave-crystal change, so no visible change) and `snow-crystals` (snow).
  Once the crystal change's `ogr-cave-crystal-01` prop row is merged, the rebake adds
  `{"ogre", "ogr-cave-crystal-01"}` and nothing else (checked by baking this branch merged with
  the crystal data).

## Proof (`$SANDOVER_DATA/shots/world-shadow-selflit/`, play look, noon, `measure.json`)
Crystal = the pixels the fix changed by more than 16 levels (opened twice; the overlay sheets
show they are the crystal blades and nothing else); RGB means over them. Ground = every pixel
more than 60 px from that mask (snow: also without the actors, which move between boots).
Mountain Pass before = the crystal change's bin (`shots/ogre-crystals/B-lit`), after = this branch merged
with the crystal data (`after-b468v3`); snow and caves before = master bin (`before-master`), after =
this branch (`after-a3`). Sheets: `sheet-ogre-noon.png`, `sheet-snow-noon.png`,
`sheet-caves-noon.png`.

| Camera | vanilla | before | after | luma / vanilla before -> after | B/G before -> after (vanilla) |
|---|---|---|---|---|---|
| pass-mid-crystal | 232.8, 144.7, 138.9 | 111.0, 72.2, 96.8 | 230.3, 137.0, 128.8 | 0.51 -> 0.96 | 1.34 -> 0.94 (0.96) |
| pass-ledge-crystal | 235.5, 162.7, 159.5 | 95.3, 78.0, 113.5 | 227.3, 149.9, 147.4 | 0.47 -> 0.94 | 1.46 -> 0.98 (0.98) |
| pass-end-crystals | 231.9, 142.0, 133.5 | 89.4, 67.5, 91.4 | 221.9, 132.8, 124.6 | 0.46 -> 0.94 | 1.35 -> 0.94 (0.94) |
| snow-crystal-crate | 78.2, 223.6, 254.0 | 46.0, 120.3, 222.7 | 97.9, 214.6, 250.8 | 0.60 -> 1.00 | |

Test: crystal luma at least 0.8 of vanilla's: fails on every old frame (0.46-0.60), passes on
every new one (0.94-1.00). On the fixed build the world map no longer touches the blades: the
mid camera with option 32 off (`after-b468v3/diag-mid-ws32off`) differs from the fixed frame
by 2.7 levels mean on the crystal (the boot-to-boot floor of these frames), against 17.3 with
the per-draw flag alone.

Ground, before -> after (mean abs difference): mid 47.65, 32.46, 26.84 -> 47.64, 32.45, 26.84
(0.45); ledge 48.00, 28.70, 23.12 -> unchanged (0.19); end 58.94, 24.41, 20.75 -> 58.93,
24.41, 20.75 (0.27); snow 62.36, 126.51, 177.54 -> 62.39, 126.55, 177.55 (0.71).

Caves (cave-crystal cameras 0020 robocave and 0022 maincave, hour 12, 3840x2160; vanilla frames reused
from `shots/cave-crystal-lights/fix`): crystal 202.7, 220.8, 241.0 and 171.7, 234.9, 252.2
before, the same after (mean abs difference 0.39 each; vanilla 177.1, 177.9, 219.8 and 142.5,
212.6, 233.9), ground difference 0.09 and 0.14. The world map does not run in sky-less levels
since the cave-crystal change, so the cave crystals were already right on master; they are on the list for any
view of them from a level with sky. The three Mountain Pass cameras on this branch without
the crystal row: whole-frame mean abs difference against master 0.00-0.40 (the texture is not
self-lit until the row lands).

Review fixes (PBR occlusion kept, mask at level load, gated on option 32), re-shot with this
branch plus the crystal data (`after-c468`, `measure-c468.json`; crystal mask = pixels the first fix
changed by more than 16 levels):

| Camera | luma / vanilla before -> after | crystal vs first fix (mean abs) | ground vs before (mean abs) |
|---|---|---|---|
| pass-mid-crystal | 0.51 -> 0.954 | 0.00 (identical) | 0.23 |
| snow-crystal-crate | 0.60 -> 0.992 | 1.38 (boot floor) | 0.44 |

The existing noon sheets show no glint on any crystal, shadowed or lit, as the classes predict.

## Cost
Not measured (perf after approval). Self-lit fragments skip the map lookup unless a PBR sun term
reads it; one uniform per texture change while the map is on; one buffer fetch per envmap
second-pass vertex. The mask is built at level load (a hash of the self-lit base positions), only
on levels with self-lit textures and only while option 32 is on: 1.4 ms for ogre.

## Follow-ups
- The actor sun map and the canopy still act on glow props when the world map is off (option 32
  0); kept so that path stays master.
