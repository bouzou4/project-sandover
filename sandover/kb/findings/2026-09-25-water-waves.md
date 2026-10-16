---
title: Can the ocean get real waves, crest and shore foam and shallow water, revision 2?
date: 2026-09-25
issue: none
status: inferred
sha: c7e249d39 (base; branch p5/water2)
---
## Answer (≤5 lines)
Yes. The near and mid ocean are tessellated on the GPU and displaced by a four-wave Gerstner
sum, with the normal and the foam taken from the same sum per pixel. The waves run out
toward the horizon with a smooth distance falloff. Shore foam, the shallow colour and depth
absorption come from the opaque scene depth under the surface, so the mid ocean now draws
after the opaque background. The maintainer picked `bold` (revision 2, C), so it is the default.
Cost: about +0.17 ms GPU for the mid ocean at 1440p MSAA 4 (434k tessellated triangles). `pc-set-water`, off by default.

## Evidence
- Revision 1's verdict: "a flat plane with a faint streak". The ocean was a flat plane
  shaded from the texture gradient.
- Formulation (research notes, GPU Gems 1 ch. 1; Catlike Coding "Waves"; Roystan "Toon
  Water"; Sea of Thieves SIGGRAPH 2018 talk):
  - Gerstner position, analytic tangents and normal. Each wave's horizontal amplitude is
    `steep / (k * n)`, so the steepness sum stays at or below 1 and crests never fold.
  - Jacobian of the horizontal displacement `(1 + dDx/dx)(1 + dDz/dz) - dDx/dz dDz/dx`.
    Crest foam where it falls below `crest_j`, above mid height, cut from a scrolling
    three-octave value noise.
  - Shore foam: a band of `shore_m` metres of vertical water depth with lines moving
    toward the shore, plus a solid contact line.
  - Shallow colour: the level colour, lightened, with `exp(-depth / shallow_m)`.
  - Absorption: the near ocean's opacity rises as `1 - exp(-absorb * path)` toward the
    level's ocean colour, where `path` is the length of the view ray under the surface.
    Past a few metres the seabed no longer shows.
  - Fresnel (Schlick, F0 0.02) toward the mood fog colour, as in revision 1.
  - Blinn glint on the wave normal. It feeds the emissive mask (bloom).
  - Subsurface: crests tinted with the shallow colour, stronger looking toward the sun.
  - Crests lighter and troughs darker (`crest_light`).
  - Foam is lit, capped below 1 and never written to the emissive mask. The non-glint
    colour is capped at 0.97.
- Positions: the ocean arrives as GS screen coordinates. The shaders rebuild the world point
  with the inverse of `camera_matrix`, then put it on the ocean plane along the pixel ray.
  The GS depth alone is too coarse far away. The plane height is the median of the
  depth-reconstructed heights of the mid vertices. The near ocean reuses the mid's value in
  the same frame, so the two meet without a step. Each point is displaced and projected
  back.
- Exception, the near ocean: its vertices carry the game's own wave heights
  (`ocean-near-add-heights`), so they are not on the plane. Sliding them along the pixel
  ray onto the plane moved them metres sideways at a grazing view. They then sampled a
  different wave phase than the mid ocean at the same place, which opened a black crack
  along the crests where the two meshes meet. That is the gap the maintainer saw in revision 2's C.
  The near ocean now uses the depth-reconstructed position, which is precise this close.
  The seam shot (`open-sea-seam-dusk`) had about 3,600 black pixels before the fix and 0
  after.
- The fragment shader needs the rest position per pixel. The ocean's `gl_Position.w` is 1,
  so the shader passes xz/w and 1/w and divides per pixel.
- Tessellation: the mid grid is 12 m (96 m tiles, 8 x 8 quads; the near grid was not
  measured). With displacement on, the ocean strips are turned into triangle lists and
  drawn as patches through `ocean_water` (vertex, tessellation control and evaluation
  stages, then `ocean_common.frag`).
  - Within 250 m each edge is cut into 0.75 m segments (up to 64); beyond, into about 3 GS
    pixels, up to `subdiv` (16). Both triangles of an edge compute the same level, so there
    are no cracks.
  - Past `range_zero_m` there is no split.
  - The barycentric split is exact: GS x, y, z, STQ, colour and fog are all linear in
    screen space.
  - An earlier version split every triangle 4 x 4 on the CPU. That was too coarse far away
    and meant a 2 to 4 MB upload a frame. It was replaced.
  - Triangle counts (`GL_PRIMITIVES_GENERATED`, `WATER-PRIMS` log, beach shoreline shot):
    - Mid ocean: 1,351 source triangles become 434,324.
    - Near ocean: 486 source triangles become 20,692.
- Range: each wave keeps full amplitude to `range_full_m` (300 m) and falls off smoothly to
  none at `range_zero_m` (1200 m), shorter waves sooner. This replaces revision 2's first cut,
  which faded everything by 150 to 220 m. That left the flat band and gap the maintainer saw in
  C.
- The mid mesh's outer-edge vertices (the ocean-map edge, where ocean-far takes over) get a
  wave weight of 0, with holes the near ocean fills excluded. At the map edge the waves
  meet the flat far band without a step.
- Scene depth: vanilla draws the mid ocean in bucket 4, before any background, with
  depth ALWAYS. That leaves no seabed depth to read. With the shading on:
  - `CommonOceanRenderer::flush_mid` keeps the mesh.
  - `OpenGLRenderer` draws it after `GENERIC_TFRAG_TEX_LEVEL1` (bucket 18), after the
    opaque tfrag and tie of both levels.
  - Just before that draw it takes a single-sample depth copy (`water-depth`, a second
    `SsaoDepth`).
  - The deferred draw tests GEQUAL against the background, with polygon offset (2, 64)
    over ocean-far.
  - The SSAO stencil and the normal-target state are set to the ocean bucket's values
    around the draw.
  - The near ocean (bucket 63) reads the same copy.
  - The mid ocean now uses this frame's camera and sun, not the previous frame's.
- Shots (new standard list, `sandover/shots/water.yaml`):
  - The beach-start shoreline at 15:00 from 1.2 m above the water.
  - At sea on the fisherman's boat route to misty at dusk.
  - The village dock at dusk with the boat.
  - 12 m over open sea on the misty route at dusk, looking into the sun: the seam check.
  - The boat ride itself is a spooled cutscene with an animated camera
    (`fishermans-boat-ride-to-misty`, entered from `fishermans-boat-docked-village` when
    `fishermans-boat-leave-dock?` holds). The rig has no hook for it, so shot 2 is a fixed
    camera on the route.
- Captures: 2560x1440 MSAA 4 with SSAO contact, bloom, actor shadows, PBR, FP16. Stored in
  `$SANDOVER_DATA/shots/ocean-waves/`:
  - Revision 2 in `r2-{off,waves,bold}`: C chosen.
  - Fix pairs: the open-sea seam shot in
    `seam-{before,after}`, and the crossing and dock shots in `r2-bold` and `r2-tess`.
  - The seam "before" is the fixed build with revision 2's wave range (full to 88 m, none at
    220 m, the old `fade_m` 220 curve). The near-ocean crack in it is the same bug C had.
- Cost: one solo run, `beach-shore-afternoon` at 2560x1440 MSAA 4, with the full look (SSAO
  contact, bloom, shadows, PBR, FP16). GPU timers (`SSAO-TIMING`), 240-frame windows,
  off / bold / off / bold. The first pair was still settling (buckets 16.7 / 10.4 ms,
  max 42 ms), so the second pair is the measurement:
  - `ocean-mid`: 0.065 ms off, 0.230 ms bold (min 0.226, max 0.251), so +0.17 ms.
  - All buckets: 3.53 ms off, 3.72 ms bold, so about +0.19 ms.
  - The near ocean has no timer in this shot's window.
  - Vertex work is cheap: about 430k triangles cost 0.17 ms. That covers the tessellation,
    the displacement and the extra shading, which is 3 to 4 times revision 1's 0.055 ms and
    within the "record, don't tune" rule.
  - Stored in `$SANDOVER_DATA/shots/ocean-waves/cost.json`.
  - The machine had no other game instance running (the script checks), but the maintainer's
    other programs may have been open.

## Parity
Not run (repo rule: parity is a milestone check). With the toggle off:
- `ocean_common` takes the `water_on == 0` branch. It feeds `position_in` through the
  unchanged vanilla math.
- `flush_mid` draws in bucket 4 exactly as before.
- The tessellation program is never bound, and no depth copy is taken.
- `SsaoDepth::capture` gained a `resolved_only` flag, false by default.
- `Shader` loads `.tesc`/`.tese` only when both files exist; only `ocean_water` has them.

## Implications for the design
- Transparent shrubs (23, 29) now draw before the mid ocean when the shading is on. None
  of the shots shows a halo.
- Crest foam is faded in the first 1.5 to 6 m from the camera. There the noise cells would
  fill the screen.
- At night (no sidecar sun), the glint and the backlit subsurface tint are off. Waves,
  foam, shallow colour, absorption and fresnel still apply.
- The tessellation stages need GL 4.0. `ocean_water` is compiled with every other shader,
  so a driver without them fails at startup, not only when the toggle is on. Not checked
  on anything but the dev machine.

## Follow-ups
- Per-level wind directions (waves running up the beach) would need a table per level.
