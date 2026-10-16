# Canopy shade: does the bake already shade the forest floor, and what counts as canopy

Question (jungle proposal): the jungle survey measured that 35% of the
up-facing ground lies 13-31 m under canopy textures, but not how much sun the original bake
already leaves there. If the bake already shaded it, a canopy term would darken it twice.

## 1. Sun under the canopy in the bake (jungle)

Data: `$SANDOVER_DATA/unbake/jungle-fit.npz` (per fitted vertex: a, k, per-slot visibility v,
fitted normal) and the jungle background glb. Ground = fitted vertices with n.y > 0.7 that are
not canopy; under canopy = a `jng-canopy-03` / `jng-canopy-hang-01` triangle lies more than 2 m
above it at zenith (1 m cells); clearing = no canopy above, inside the canopy's bounding box.
43,043 vertices under canopy, 50,258 in clearings. Sun share = k v max(0, n.d) lgt over that plus
a amb, in luminance, per palette slot.

| slot | sun y | v < 0.5 (under / clear) | mean v | mean sun share | mean sun share without v |
|---|---|---|---|---|---|
| 0 dawn | 0.10 | 0.21 / 0.31 | 0.79 / 0.72 | 0.18 / 0.16 | 0.25 / 0.26 |
| 1 morning | 0.50 | 0.27 / 0.40 | 0.70 / 0.62 | 0.33 / 0.33 | 0.43 / 0.49 |
| 2 noon | 0.95 | 0.54 / 0.27 | 0.50 / 0.73 | 0.27 / 0.46 | 0.47 / 0.57 |
| 3 afternoon | 0.52 | 0.35 / 0.43 | 0.66 / 0.61 | 0.22 / 0.31 | 0.34 / 0.47 |
| 4 dusk | 0.11 | 0.17 / 0.31 | 0.83 / 0.71 | 0.08 / 0.11 | 0.13 / 0.21 |

Noon baked luminance: 89 under canopy, 136 in clearings. Noon v under canopy is spread, not
dark: 13% below 0.1, 19% above 0.9 (clearings: 55% above 0.9). Mean noon v does not change with
the canopy height above the ground (0.47-0.51 for 2-13, 13-20, 20-31, 31+ m). The fitted sun gain
k is also lower under canopy (0.39 vs 0.59 in every slot).

Reading: the bake shades the forest floor partly and softly at noon (about a third less light,
sun share 0.27 vs 0.46) and not at all in the morning, afternoon and dusk slots, where the floor
under the canopy gets as much or more baked sun than the clearings. So the premise holds with a
caveat: at noon the canopy term removes the remaining share (0.27 on average), which stacks on
the bake's own soft shade; with a lower sun it is the only shade there is. The term removes only
the fitted sun share (as actor shadows do), so vertices the bake left dark are not darkened again.
The measuring script was a one-off and is not in the repo.

## 2. Canopy from data

Rule (`sandover-unbake canopy`, `unbake/canopy.py`): foliage held up by a tree. Candidate
textures: class `foliage` with an alpha that is at least 5% cut out. A triangle is canopy when
|n.y| >= 0.5, it sits at least 4 m above the highest ground (up- or down-facing triangles of every
other texture) within 2 m sideways, and a steep wood surface within 10 m rises to within 6 m
below it. A texture counts when such triangles cover 25 m2 and 80% of its clear area. Without
the trunk test, cliff-edge grass fringes (`rol-grass-fringe`, `v2-grass-ground-hirez-fringe`,
`snow-grassclump`) and hanging greenery passed; without the sideways ground test, more did.

Result per level (coverage m2 at 0.5 m texels):

| level | canopy textures | coverage m2 |
|---|---|---|
| jungle | jng-canopy-03, jng-canopy-hang-01, vil1-palm-leaf-colorful | 18,031 |
| village1 | vil1-palm-leaf-colorful | 2,384 |
| beach | bch-palm-leaf-colorful, bch-palmtree-beard, vil1-palm-leaf-colorful | 1,950 |
| rolling | vil2-bonzailarge-02 | 1,005 |
| training | jng-palm-leaf, vil1-palm-leaf-colorful, vil1-palmtree-beard | 1,650 |
| firecanyon | bch-palm-leaf-colorful, bch-palmtree-beard | 646 |
| village2 | v2-pine, vil2-bonzailarge-02, vil2-wallgrass-02 | 2,035 |
| snow | snowtree04-07, snowtree-branch-middle | 1,497 |
| ogre | org-pineleaf-bigall-01 | 4,750 |
| all others | none (swamp below the 100 m2 floor) | |

The jungle survey's two hand-named canopy textures are exactly the jungle's main picks. Suspect:
`vil2-wallgrass-02` in village2 (grass on walls next to wooden houses). Unverified: whether the
snow and ogre pines read well with this term (no capture). `v2-bonzai-bigcapopy` in rolling,
named by the jungle survey, does not pass: only 20 of its 306 m2 are 4 m clear of the ground
(below the 25 m2 floor; it is held by a trunk). `vil2-wallgrass-02` passes on 54 m2.

## 3. Revision 2 model (after canopy-001)

Revision 1 measured as a near-uniform dark slab (B/A 0.33-0.45 on the dense path, 37-49% of the
original), shading side-lit trunks and foliage, with a hard zig-zag edge at 16:00. Causes: cover
was a binary any-opaque-sample count that saturates under stacked layers; the term removed the
whole remaining sun share regardless of the bake; the map was sampled bilinearly at 0.5 m with a
two-step projection over an unsmoothed height; shared triangle edges were counted twice
(sample centres on the edge), and steep hanging skirts (`jng-canopy-hang-01`, |n.y| about 0.6)
drew thin concentric arcs.

Changes: map at 0.25 m, cover = 1 - product over layers of (1 - 0.9 alpha), two-sided duplicates
dropped, samples kept off shared edges, canopy cards need |n.y| >= 0.7, height channel blurred
(1.5 m). Shader: mip-filtered lookups, penumbra max(0.3 m, 0.03 x distance to the leaves along
the ray), local mean over 6 m. Area level m = 1 at high sun (the bake already holds the canopy's
shade there; smoothstep on sun y 0.8-0.95), else max(mean transmittance, 0.5); dapple =
1 + 0.8 (T - Tmean) / max(Tmean, 0.3); new sun visibility = bake visibility x m x dapple, so the
term can brighten leaf gaps. Foliage receivers x 0.3; actors use m without the bake.

Offline prediction (jungle fit vertices, up-facing ground under canopy, B/A of lighting):

| sun | mean | p5 | p50 | p95 | std |
|---|---|---|---|---|---|
| noon (slot 2) | 0.989 | 0.85 | 1.00 | 1.13 | 0.088 |
| 16:00 (3/4 slot 3 + 1/4 slot 4, y 0.45) | 0.973 | 0.81 | 1.00 | 1.00 | 0.066 |
| 9:00 (y 0.69) | 0.929 | 0.73 | 0.99 | 1.00 | 0.098 |

Clearings: 0.999 at noon, 0.957 at 16:00 (tree shade falls into them with a lower sun).

## 4. Revision 3 model: canopy light (after the first canopy review)

The maintainer: "The shots picked for canopy shade are terrible I literally cant see the ground in the
main forest area", and on effects generally: "might as well be a spot the difference game". The
revision-2 model was bounded by the bake's remaining sun share (predicted 2-5% at the new cameras).

Now: the mean cover above a point (6 m) cuts the sky light (option 12, default 35%) and cools it
(option 15); the sun reaches the point only through the leaf gaps at its projection toward the
sun (cover over about a metre), replacing the bake's own sun visibility under the canopy (relight
capped at 4x). Map gap structure in the main forest (255-325, -345 to -285 m): after a 1 m filter,
25-40% of the canopy floor has transmittance above 0.15-0.25, in patches of about 2 m2.

Predicted at the reshoot cameras (ground in view, A ground about 97 of 255): forest-floor noon
shade -29 levels, patches +19 (B1); B2 -40 / +15; 96% of the floor changes by more than 10 levels.
At 14:00: -23 / +6 (B1), -33 / +4 (B2).

## 5. Revision 4: one light multiplier for every surface (after canopy-forest-003)

Captured revision 3: the dirt path went near black while the grass beside it under the same canopy
did not change. Mechanism (code read): the jungle grass `jng-leafyground` is class foliage, and
the receivers gave foliage 0.3 of the effect; the dirt took the full factor, whose sun part was
scaled by the bake's sun share (large on the dirt) and whose sun visibility replaced the bake's,
so dirt in shade kept only (1 - share) x 0.65 of its light. Jak took the same sun-part cut.
Fix: under the canopy every surface (and Jak) gets the same multiplier, independent of texture,
class and bake: shade = today's light x (1 - option 12) shifted cool, sun patches = today's light
x (1 + option 16) warm; the cover above the point fades it in. forest-edge was mostly outside the
canopy map and is dropped.
