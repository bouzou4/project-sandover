# Terrain sun occlusion of actors: agreement with the bake (2026-10-01)

Feature: shadow option 20 (`kb/howto/shadows.md` item 7). Revision 1 (sections up to "Revision 2")
tested the heightfield per pixel along the sun and was rejected; revision 2 (last section) uses
cover overhead per actor. The world keeps its baked shadows. The risk is disagreement: Jak lit on baked-dark ground, or dark on
baked-sunlit ground. Measured offline before any capture.

## Method

- Heightfield: a scratch copy of the infra builder from each level's collision mesh (fr3), triangles
  with `nolineofsight` dropped (invisible walls), top layer, 1 m texels with max mips; the march
  copied from the shader (10 taps at 1.6^k m, footprint 0.25 t, soft over ±0.5 m).
- Ground: uniform points every 0.5 m on walkable collision (mode ground, normal y > 0.7), 0.3 m
  up; "covered" = the top layer is 1.5 m or more above the point and 0.6 m around it.
- Bake visibility, two estimates from the lighting sidecar (`unbake/<level>-fit.npz`), averaged
  over the up to four ground vertices within 1 m:
  - `v`, the fit's per-vertex per-slot visibility. Unreliable where a vertex is shaded in every
    slot: the per-vertex sun gain k absorbs a permanent shadow and v stays near 1 (the edge spot of
    `shots/terrain-sun.yaml`: v 0.98 on ground whose baked luminance is half its sunlit neighbour's).
  - albedo-free ratio (used below): the vertex's luminance in the slot over its night-slot
    luminance, placed between the ratios an ambient-only and a fully lit vertex would have (fitted
    ambient and sun of both slots, n.L of the vertex). Only for levels with a real night palette;
    firecanyon, rolling and ogre have none, so they are left out.
- Disagreement: |heightfield visibility - bake visibility| > 0.5. "1 m tolerant": also more than
  0.5 at the four points 1 m around, so a shadow edge off by a metre does not count.
- Slots: day slots with the sun at least 3° up and a fitted sun of 120 u8 or more.

## Results

| level | slot | sun elev | ground pts | covered | open: disagree | open, 1 m tolerant | open: hf shade / bake sun | open: hf sun / bake shade | covered: bake sunlit |
|---|---|---|---|---|---|---|---|---|---|
| village1 | 0 | 10 | 16089 | 0.34 | 0.35 | 0.30 | 0.24 | 0.11 | 0.33 |
| village1 | 1 | 37 | 35940 | 0.36 | 0.26 | 0.20 | 0.17 | 0.09 | 0.27 |
| village1 | 2 | 71 | 37338 | 0.36 | 0.22 | 0.15 | 0.11 | 0.11 | 0.20 |
| village1 | 3 | 33 | 35077 | 0.36 | 0.29 | 0.24 | 0.20 | 0.09 | 0.20 |
| village1 | 4 | 8 | 7877 | 0.42 | 0.17 | 0.13 | 0.11 | 0.07 | 0.25 |
| beach | 0 | 11 | 22443 | 0.25 | 0.32 | 0.26 | 0.24 | 0.08 | 0.48 |
| beach | 1 | 39 | 31410 | 0.25 | 0.17 | 0.12 | 0.11 | 0.06 | 0.35 |
| beach | 2 | 75 | 32283 | 0.25 | 0.15 | 0.09 | 0.07 | 0.08 | 0.28 |
| beach | 3 | 31 | 30509 | 0.26 | 0.23 | 0.19 | 0.18 | 0.05 | 0.25 |
| beach | 4 | 5 | 6522 | 0.27 | 0.22 | 0.17 | 0.14 | 0.08 | 0.16 |
| jungle | 0 | 6 | 24162 | 0.35 | 0.47 | 0.45 | 0.46 | 0.00 | 0.39 |
| jungle | 1 | 30 | 47121 | 0.30 | 0.34 | 0.29 | 0.29 | 0.04 | 0.19 |
| jungle | 2 | 73 | 48554 | 0.30 | 0.31 | 0.21 | 0.11 | 0.20 | 0.15 |
| jungle | 3 | 31 | 42367 | 0.28 | 0.30 | 0.27 | 0.27 | 0.03 | 0.32 |
| jungle | 4 | 6 | 9538 | 0.21 | 0.51 | 0.50 | 0.50 | 0.01 | 0.43 |
| training | 1 | 26 | 34783 | 0.23 | 0.22 | 0.19 | 0.20 | 0.02 | 0.15 |
| training | 2 | 72 | 39520 | 0.22 | 0.24 | 0.14 | 0.12 | 0.12 | 0.14 |
| training | 3 | 28 | 35227 | 0.22 | 0.23 | 0.18 | 0.18 | 0.04 | 0.14 |
| finalboss | 1 | 36 | 132 | 0.05 | 0.10 | 0.06 | 0.07 | 0.02 | - |
| finalboss | 2 | 63 | 142 | 0.04 | 0.12 | 0.07 | 0.09 | 0.03 | - |
| finalboss | 3 | 37 | 128 | 0.03 | 0.09 | 0.07 | 0.08 | 0.01 | - |

Covered ground (interiors, under roofs and overhangs, 20-36% of walkable ground in the villages)
is baked sunlit at 14-48% of points (noon: 14-28%), with the fit's `v` 27-60% (unreliable
there, see above). Village1 hut floors at noon: baked luminance 76 against 139 on open ground; at
dawn 55 against 56 (the low sun adds nothing to flat ground in the bake either).

## Why it disagrees where it does

1. Edges: a metre of tolerance removes a quarter to a third of the disagreement at noon. Sources:
   1 m texels, the max footprint widening distant occluders (0.25 t: 2.5 m at 10 m), vertex
   interpolation of the bake across long ground triangles.
2. Long shadows of tall terrain at low and mid sun: the heightfield shades ground that the bake
   leaves lit (the "hf shade / bake sun" column grows as the sun drops: village1 0.11 at noon,
   0.20 at 33°, 0.24 at 10°). Cutting the march at 5 m instead of 80 m (exact 0.5 m march) only
   brought village1 slot 0 from 0.34 to 0.19, so near occluders count too. Hypothesis: the
   original bake's sun shadows are short or partial (it shades the jungle floor under canopy only
   at noon, `2026-09-30-canopy-shade.md`).
3. The fitted sun sits 6-12° lower than the bake's shadows suggest: with the mood-table direction
   (`dir_table`) instead of the fit, open-ground disagreement (fit's `v`) falls from 0.27 to 0.19
   (village1 slot 3) and 0.20 to 0.15 (beach slot 3); a search over directions finds its best 6-12°
   higher. The feature keeps the fit, as the actor shadow map does, so the two agree with each
   other; raising the fit is a separate change to the sun both use.
4. Jungle: foliage is not collision. At noon the bake is darker than the heightfield under the
   canopy (0.20 "hf sun / bake shade"); the canopy map covers that. At 6° the march reaches
   tree trunks and rock across the level (0.46-0.50).
5. Overhangs and arches: one top per texel shades everything under them; the albedo-free bake
   shows those floors mostly unlit, so this agrees more than it disagrees.

Nothing was tuned per level. The footprint was narrowed from the infra helper's 0.5 t to 0.25 t
(same ten taps): open-ground disagreement 0.25 -> 0.19 (village1 noon) and 0.17 -> 0.12 (beach
noon) on the fit's `v`, with 0.1-1.6% of points losing an occluder the exact march finds.

Scratch scripts (not committed): `agree2.py`, `lhsim.py`, `proxy.py`.

## Revision 2: per-actor cover (after the first round was rejected)

On revision 1: "shot 2 of 3 has a weird cutoff on jaks shadow in B. In shot 1 B shows no
shadows? isnt there two light sources off screen?"

What went wrong in revision 1:
- Edge shot: the sun term was applied per receiving pixel with a sharp result. The heightfield
  said the ground at Jak's feet was out of the sun, so the near part of his cast shadow was
  removed and the far part kept, on grass the bake paints sunlit; Jak himself went dark on bright
  ground. That is the open-ground disagreement measured above (17-29% in village1).
- Hut shot: the manifest ran with `effects: off`. The hut lamps (`hutlamp`, gate slot 5) light
  through palette slot 5, which only `update-mood-flames` drives (`update-mood-village1`,
  mood.gc:766, guarded by `*time-of-day-effects*`); with effects off the slot is 0, so the
  local-light selection fell to 0 lights before the shot (revision 1's B log: `0 lights`). In play
  the slot sits at 0.5 plus flicker at every hour, so the lamps are always lit: the A log of
  revision 1 lists, in the first frames after the load, lamps 1196 and 1197 at 6.7 and 7.7 m from
  Jak with 0.24 and 0.17 of light at him, both casting, and the fireplace spawner at 8.8 m; they
  fade to 0 lights before the shot.
  The landing disc was drawn but invisible: 0.45 m radius at 0.3 strength, fully dark only
  inside 0.18 m, which is under his feet. The game also blends in a per-hut light kit there
  (`set-target-light-index 7` around (-20.5, -74.5) m, mood.gc), which the merc lights keep.

The rule now (`kb/howto/shadows.md` item 7): one cover value per actor, from the top layer around
its root (share of 41 points within 4 m with solid world 1.5-40 m above it, eased from 75% to
95%), applied to the whole actor and to its whole cast shadow (an R8 colour channel in the sun
map). Only cover overhead counts: open-ground shading from walls and cliffs is left out, because
that is where the bake disagrees most. Across a doorway the share of points under the roof falls
over about 3 m; the 75-95% ease puts the fade in the last metre before the roof edge, where the
bake turns (village1 hut porch: cover 0.96 / 0.70 / 0.18 / 0 over 1 m; bake visibility 0 / 0 /
0.42 / 0.67).

Agreement with the bake, walkable ground, albedo-free bake visibility (the method above), the
fraction of ground the rule darkens (cover > 0.5), how much of that the bake paints sunlit, and
that as a share of all ground. Open ground the rule does not darken is unchanged from today by
construction.

| level | slot | sun elev | ground darkened | of it, bake sunlit | ground wrongly darkened |
|---|---|---|---|---|---|
| village1 | 0 | 10 | 0.19 | 0.33 | 0.064 |
| village1 | 1 | 37 | 0.23 | 0.18 | 0.041 |
| village1 | 2 | 71 | 0.23 | 0.11 | 0.026 |
| village1 | 3 | 33 | 0.23 | 0.15 | 0.035 |
| village1 | 4 | 8 | 0.25 | 0.30 | 0.074 |
| beach | 0 | 11 | 0.13 | 0.44 | 0.059 |
| beach | 1 | 39 | 0.14 | 0.25 | 0.034 |
| beach | 2 | 75 | 0.14 | 0.19 | 0.026 |
| beach | 3 | 31 | 0.14 | 0.22 | 0.031 |
| beach | 4 | 5 | 0.12 | 0.19 | 0.023 |
| jungle | 0 | 6 | 0.23 | 0.35 | 0.082 |
| jungle | 1 | 30 | 0.18 | 0.15 | 0.027 |
| jungle | 2 | 73 | 0.18 | 0.11 | 0.019 |
| jungle | 3 | 31 | 0.15 | 0.34 | 0.051 |
| jungle | 4 | 6 | 0.10 | 0.46 | 0.046 |
| training | 1 | 26 | 0.09 | 0.09 | 0.008 |
| training | 2 | 72 | 0.09 | 0.07 | 0.006 |
| training | 3 | 28 | 0.09 | 0.12 | 0.011 |
| misty | 1 | 39 | 0.17 | 0.02 | 0.003 |
| swamp | 0 | 56 | 0.32 | 0.01 | 0.002 |

Before (revision 1, per pixel along the sun): 9-24% of open ground wrong at mid and high sun. Now:
0.6-4% of all ground wrongly darkened at mid and high sun, 2-8% at dawn and dusk. The dawn and
dusk shares are the least reliable: the bake adds little sun to flat ground at a low sun, so the
day/night ratio is noisy there.

Choices measured on the way: a 15 m height limit missed the village1 hut (built into the hill,
the top layer above it is 20-34 m up) and a 30 m limit half-covered it; with no limit beach's
wrongly darkened share at noon rose from 2.6% to 4.6%. Adding the sun ray to the overhead test
changed nothing (a roof all around already blocks it). Easing from 50% instead of 75% widens the
doorway fade to about 2 m but darkens 30-70% more ground the bake paints sunlit.

Collision gaps: village2's blue sage hut has no roof collision (top layer 0 m above the floor),
so its interior reads as open; the warrior's and gambler's hut sits under rock that tops the
column 80-108 m up and reads as open too. Firecanyon and rolling show large covered areas
(firecanyon's lava under the road; rolling, not inspected) and were not measured (no night
palette for the ratio).

## Revision 3 (after the revision 2 review, 2026-10-07)

On revision 2's full cover (C): "in hut-1900 shot C it doesnt make sense jak is standing in
front of a light on the wall he shouldnt be darkened to the point as if theres no light" and
"in hut 1300 C I can literally see the ring of his shadow as if the inner part of his shadow was
masked out but not the feather around it."

What lights an actor under cover (merc2.frag, from the code): the vertex light is the game's rig,
`ambient + light 0 + light 1 + light 2` (merc2.vert); `vtx_sun` is lights 0 and 1. The cover
removed `cover x vtx_sun` (and the same share of the PBR key light); ambient, light 2 and the
local lights (`shadow_actor_lights`, up to four, the hut lamps among them, soft-limited to 0.6
luma) were untouched, and the tile pass skips characters while local lights are on. So the lamps
and hearth did still reach Jak, with 0.24 and 0.17 of light at him (revision 1's log).

Why he went dark anyway: in Jak's hut lights 0 and 1 are not the sun. `update-mood-village1`
(mood.gc) calls `set-target-light-index 7` within 15 m of (-20.5, -74.5) m, and
`make-village1-light-kit` (mood-tables.gc) makes group 7 with `make-light-kit`: a fixed warm key
and fill (0.8, 0.775, 0.7) at 0.9 each plus a down light at 0.4, the same at every hour. The
time-of-day rig is light group 0, filled by `update-mood-palette` from the mood lights table
(lights 0 and 1 are the two blended sun snapshots, light 2 has level 0). Removing lights 0 and 1
of kit 7 took away the hut's designed interior lighting, about four times the lamps' light, so
Jak read darker than in the original. Fix: cover now scales by the sun's share of the rig, read
from light 2: a rig whose third light carries 15% or more of lights 0 and 1 is a kit and keeps
its lights (`Merc2::sun_share_of_rig`); a blend between the two (the kit fades in over 4 m)
gets the share in between. In Jak's hut the cover no longer touches his lighting; only his sun
shadow on the floor goes. NPCs with their own light index (mayor 2, bird lady 4, sculptor 3:
levels/beach/*.gc) keep theirs the same way.

The ring: receivers faded the actor shadow by the caster channel sampled once at the centre tap
(`texture(tex_T21, t.xy)`). The outer PCF taps of the penumbra compare texels at the caster's edge
while the centre sample falls outside the silhouette, where the channel is cleared to 1, so the
feather kept full strength around a removed core. Now each tap multiplies its own occlusion by
the smallest of the four texels its depth compare used (`textureGather`), so core and feather
fade together; with option 20 at 0 every caster writes 1 and the result is today's.

Jungle tunnel (revision 2, not shown): B2 kept Jak's sun shadow and lighting, so the
in-game cover there read 0 while the offline cover is 1.0 (roof 7 m up, no taller surface within
4 m). Not explained from the code; the pre-pass log line now prints `terrain`, `maps` and
`cover at Jak` every 240 frames so the next capture shows which input is missing.
