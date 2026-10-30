# Water transparency (near ocean): what hides submerged objects, and the arms so far

Chosen: A, 2026-10-22. Baked as the one default: absorb
0.03 per metre, opaque by 12 m (`opaque_m`), per-channel absorption 1.6 / 0.9 / 0.5, the fill
from the game's unshaded water colour, and the depth re-copied before the near ocean. The
`actor_depth` and `absorb_rgb` switches (106, 107) and the `transparency/*` presets are gone:
with water on, this is the only path; with water off the near ocean draws upstream's single
alpha pass. The old values (0.6 per metre, opaque by `shallow_depth_m`) survive only in the
tables below.

Play report (jungle-start cave mouth, night, hour 21.3): the water is too opaque. Jak's legs
barely read, and the orbs under the surface show only by their glow rings. Status: measured and
mechanism found. The first three arms kept no colour (constraint 2 below); a second set followed and A was chosen (above).

Shots: `sandover/shots/water-transparency.yaml`. The snapshot camera at night and at noon, plus
the beach-start shallows at noon. Vanilla is shot with no forms; looks with `--play-look`, wave
clock pinned. The rig's frozen actor state has the orbs on the bed left of Jak, with no glow
rings.

## Mechanism (V = verified in code)

- V: the near ocean (bucket OCEAN_NEAR, 63) reads a depth copy taken after
  GENERIC_TFRAG_TEX_LEVEL1 (18), before the actors (pris buckets 48-56) are drawn
  (`OpenGLRenderer.cpp`, `m_water_depth.capture`). So a swimmer or an orb is fogged by the depth
  of the seabed behind it, not by its own depth under the surface.
- V: transmission is one scalar. `color.a = mix(a, 1, max(1 - exp(-absorb * path), deep))` with
  the fixed blend SRC_ALPHA / ONE_MINUS_SRC_ALPHA (`ocean_common.frag`, bucket 0). The bed is
  lerped toward the shaded surface colour by one alpha, so every channel fades at the same rate.
  Hue is lost because the surface colour, not the bed's colour, wins.
- V: the surface colour itself is a brightened, saturated blue. The shallow tint is
  `min(mix(lum, rgb, 1.25) * 1.9 + 0.02, 0.95)`, then the toon tonemap. Measured water around
  the orbs at night: vanilla rgb (16, 44, 80), today (3, 55, 140).
- The terms (orbs at night, share of vanilla's visible contrast, regression on luma, scalar
  path): today 0.02. Removing the opacity floor alone (`deep`, opaque by shallow_depth_m
  1.75 m) gives 0.02. The 0.6/m absorption over the path to the seabed already removes them.
  Absorption alone, with the floor off: 0.15/m 0.23, 0.08/m 0.43, 0.03/m ~0.7-1.0, 0 ~1.0. Both
  terms remove the orbs on their own, because both read the seabed depth.

## Engine change on `look/water-transparency` (experiment switches, removed once A was chosen)

`pc-set-water` options 102 `opaque_m` (opacity depth, separate from the tint depth), 103-105
`absorb_r/g/b` with 107 `absorb_rgb` (per-channel transmittance), 106 `actor_depth` (depth
re-copied before OCEAN_NEAR), and view 21 (dry: the near ocean covers nothing, the chroma
reference). The per-channel path draws the near base twice. Pass 1 multiplies the scene by
T / max(T). Pass 2 is the normal over blend with alpha 1 - max(T). A first version added the
in-scatter (ONE, ONE) and doubled up on folded wave crests, which showed as bright blue
shards. The over blend fixed it. A few faint streaks remain in A.
T = (1 - vanilla alpha)(1 - foam)(1 - smoothstep(0, opaque_m, depth)) exp(-absorb * absorb_rgb * path).
The depth fog stays in every arm.

Arms (presets `transparency/A|B|C` in water.toml, deleted once A was chosen; all actor_depth 1, absorb_rgb 1, tint
1.6/0.9/0.5): A absorb 0.05, opaque 10 m. B 0.25, 5 m. C 0.6, 3.5 m (today's rate, floor
deeper).

## Measurements, masked orbs (dry frame = 1)

`t` = orb-minus-bed luma contrast over the dry frame's. `hue` = the orb's Oklab (a,b) relative
to the water around it, projected on its dry hue, over the dry chroma.

| frame | night t | night hue | noon t | noon hue |
|---|---|---|---|---|
| vanilla | 0.36 | 0.43 | 0.21 | 0.28 |
| today | 0.03 | -0.02 | 0.09 | -0.06 |
| A | 0.03 | 0.08 | 0.07 | 0.06 |
| B | 0.01 | -0.04 | 0.03 | -0.02 |
| C | 0.21 | -0.03 | 0.07 | -0.04 |

By eye, A shows the orbs, Jak's legs and the submerged rock fading into the water with no hard
edge at the cave spot. They read only as purple on blue: luma contrast and hue are both near
zero. The C night `t` comes from the ripple ring. Jak's pose differs per shot, so Jak-leg ROIs
are not comparable. The beach and noon mesh-edge check is not done (frames are in the work
folder).

## Why the arms fail constraint 2, and the next step

T starts at (1 - vanilla alpha). The in-scatter is today's bright toon surface colour, which
is about 1.8x vanilla's at night. Any bed colour that survives is buried under it. The next
change should:
1. Use the water's own extinction colour for the in-scatter, darker than the shaded surface:
   keep the reflection and glint as they are, and add in-scatter as `deep_colour * (1 - T)`
   instead of `S * (1 - T)`.
2. Leave the shallow tint (x1.9) out of the in-scatter. With `actor_depth` it now also reads a
   swimmer's depth.
3. Then re-tune A to vanilla's t 0.36 / hue 0.43 on the night orbs, and run the beach and noon
   edge check before comparing again.

## Second set of arms (darker fill)

The per-channel path's in-scatter now fills with the game's own unshaded water colour (vertex
colour x texture). The shaded surface (shallow tint x1.9, lighting) weighs in only through
fresnel and foam. Arms: A absorb 0.03 / opaque 12 m; B 0.3 / 6 m (half-way between A and
today); C 0.6 / opacity depth = shallow_depth_m (today's rate, new fill). All keep the depth
fog, the pre-ocean depth copy and the per-channel tint 1.6/0.9/0.5. D is today.

| frame | night t | night hue | noon t | noon hue |
|---|---|---|---|---|
| vanilla | 0.35 | 0.43 | 0.20 | 0.27 |
| today | 0.06 | -0.02 | 0.10 | -0.06 |
| A | 0.03 | 0.15 | 0.08 | 0.10 |
| A2 (0.06 / 8 m, not in the round) | 0.01 | 0.03 | 0.02 | 0.05 |
| B | 0.27* | -0.03 | 0.07 | -0.04 |
| C | 0.06 | -0.03 | 0.06 | -0.04 |

*Ripple ring, not the orbs.

Open water around the orbs at night: vanilla rgb (10, 44, 92), A (6, 47, 100), today
(2, 48, 114). The fill brings the surface back to vanilla's level without flattening it: the
reflection, glint and foam stay. Mesh edges: none seen in A, the most transparent arm, at the
cave (night, noon) or the beach.

Where A stops: the orbs read with their full structure. They stay about as bright as the water
(t 0.03), and their pink shifts to blue-purple (hue kept 0.15 against vanilla's 0.43). Lowering
absorption further would mean dropping the depth fog. The remaining loss is not the fill. It
comes from the per-channel tint (red 1.6x) over the path to an orb several metres down, and
from the toon tonemap applied after compositing. The dry frame's orbs are already more
saturated red than vanilla's. The next lever is a weaker red tint (e.g. 1.2/1.0/0.8). Checking
whether the toon pass desaturates blue-tinted mid-tones needs a capture with the tonemap at
clamp.
