# World sun shadow map (2026-10-08)

Problem: unshadowed grass next to a correctly shadowed Jak breaks the look. Today only actors cast into the sun map; the world's own
sun shadows come from the bake, and five consumers approximate sun visibility with the level
heightfield. This note is the design of a real sun shadow map of the static world.

## Approach

- Casters: the static world of every loaded level, drawn depth-only from the sun: tfrag (normal,
  trans, dirt, ice trees; not lowres or water), tie (normal, trans and their envmap base draws;
  wind ties with their unbent instance matrix) and shrubs, geometry LOD 0. Alpha-tested draws keep
  their alpha: the caster shader samples the draw's texture and discards below the draw's alpha
  reference (or below half for plain alpha blending); additive and subtractive draws, and draws
  that write no depth, cast nothing. Merc and generic actors stay in the existing actor sun map.
- Direction: the sun disc's model: the day slots' fitted directions placed at their peak
  hours and slerped by the game hour, so the map moves on the same smooth arc as the disc; the
  palette blend when the hour is unknown. The actor sun map takes the same direction while the
  world map is on, so Jak's shadow and a tree's agree. Hidden at night: the map's strength follows
  the day weight of the palette (0.05 to 0.3) and the sun's elevation (fades out below about 2°).
- Filter: percentage-closer with a blocker search (PCSS-lite): 8 raw taps find the mean blocker
  depth, the penumbra grows with the blocker-to-receiver distance (0.012 per metre, never under
  1.5 texels), then 16 Poisson taps of the hardware 2x2 compare. No texel edges; contact shadows
  stay crisp, a crown's shadow 10 m down is about 12 cm soft.

## Cascades

Two to three cascades in one depth array (32-bit float), sized by the shadow-quality tier (shadow
option 40 sets options 35 and 36 from this table; the default is high):

| Tier | Map size | Cascades | Splits (m from the camera) | Texel of the last cascade |
|---|---|---|---|---|
| low | 1024 | 2 | 0.5-20-120 | about 19 cm |
| medium | 2048 | 3 | 0.5-10-35-120 | about 9 cm |
| high | 4096 | 3 | 0.5-10-35-120 | about 5 cm |

Each cascade is an orthographic box around the bounding sphere of its slice of the view frustum
(the sphere depends only on the split and the field of view, so the texel size never changes as
the camera turns), its centre snapped to whole texels in light space (no shimmer). Depth clamp is
on, so a caster nearer the sun than the box (a cliff behind the camera) still lands at depth 0.
A receiver takes the first cascade that holds its PCF footprint and blends into the next over the
outer 10% of the box; past the last cascade the map fades out and the bake alone remains. Map size
and cascade count are `pc-set-shadow-option` 35 and 36, so the settings page's shadows
low/medium/high row can set them.

## Shadow only the bake's direct share

The sidecar fit models each vertex per palette slot s as `C_s = a·Amb_s + v_s·k·max(0, n·L_s)·Lgt_s`
(u8 units, `tfrag3_sun.vert`). Per vertex, from the live palette weights w_s (luminance):

- `σ = Σ w_s·v_s·k·max(0, n·L_s)·Lgt_s / shown`: the direct-sun share of the colour as baked
  (today's `shadow_share`; zero where the bake has the vertex in shadow, v_s = 0).
- `σ1 = Σ w_s·k·max(0, n·L_s)·Lgt_s / shown`: the share the same vertex would have fully lit.

With V the map's sun visibility at the pixel (1 lit, 0 shadow; the minimum of the world map and the
actor map), the receiver keeps `1 − strength·max(σ − V·σ1, 0)` of its colour. The direct light
becomes `min(v_bake, V)` of the potential: where the bake is lit (σ = σ1) the map removes
`(1 − V)·σ`; where the bake is already in shadow (σ = 0) nothing is removed whatever the map says;
in a baked penumbra (v = 0.5) the map removes only what the bake left (a map penumbra over a baked
penumbra does not double). The sky-hued shadow split and the emissive gate `1 − E` apply as
today. Receivers without sidecar data (shrubs, envmapped ties, wind ties, trees with q = 0) have no
v: they use `σ = σ1 = clamp(1 − ā·lum(Amb)/lum(C), 0, 1)`, the colour above what the level's median
ambient gain ā (from the sidecar records) and the live fitted ambient give; a vertex the bake
leaves at ambient comes out at 0. On village1's fit at 15:00 this estimate follows σ with a
correlation of 0.84 and a mean difference of 0.11 on tfrag and tie vertices facing the sun
(`share_check.py` below); for shrubs it is an assumption (their sidecar data is not shipped; the
fit has them, at r² 0.41).

Known risk, measured offline: the fit's v is unreliable where a vertex is shaded in every slot
(k absorbs the permanent shadow, `2026-10-01-terrain-sun.md`). On ground under any mesh 2.5 m or
more above it (a crude cover proxy that also counts crowns), σ at 15:00 has a median of 0.15-0.22
and a 90th percentile of 0.43-0.48 (village1, tfrag and tie), against 0.38-0.56 in the open: under
a roof the map can still remove up to that share. The frames decide whether that reads as double
darkening; the A/B measure below checks it.

## Bias

- Casters: slope-scaled polygon offset (factor 1.5, units 2) and back faces culled off (foliage
  cards are two-sided).
- Receivers: receiver-plane depth bias, the depth slope across the shadow map from the screen
  derivatives of the receiver position, applied per tap, so wide kernels on slopes stay clean;
  plus a normal offset of one texel of the chosen cascade along the geometric normal, turned
  toward the sun (two-sided cards), and a constant of half a texel. The offset is about 1 cm in
  the near cascade, so feet keep their contact (no peter-panning).

## Per frame, not per slot

| | Per palette slot, cached | Per frame, cascades |
|---|---|---|
| Direction | five fixed slot directions, crossfaded | the smooth arc of the disc |
| Coverage | whole level at 10-20 cm texels (4096²) | 1-9 cm near the camera |
| Memory | 64 MB per slot, about 320 MB for five | 2048² x 3 x 4 B = 48 MB (medium) |
| GPU per frame | none after load | depth-only redraw of what each box holds |

Geometry per level (fit meshes, LOD 0): village1 0.53 M triangles (tie 0.37, shrub 0.10, tfrag
0.06), beach 0.40 M, jungle 0.53 M, misty 0.67 M, rolling 0.78 M, swamp 0.74 M; two levels are
loaded at once. The BVH culls each box: the near cascades hold a few percent, the last one (about
190 m across) roughly a third, so a frame draws about 0.5-0.8 M depth-only triangles (estimate:
0.3-0.6 ms GPU at 4K-class rates; the pre-pass timer query logs the real cost, see the howto). The
slot cache cannot follow the arc and is too coarse at Jak's feet, so per frame it is; caching the
far cascade (redraw only when its snapped origin moves or the sun turns by a texel) is the item for
the optimization pass.

## What each heightfield consumer becomes

| Consumer | Today | With the map |
|---|---|---|
| Roof cover for actors (option 20) | five rays per actor through the heightfield, one value per actor | the map per merc pixel (option 33): `occ = max(actor map, world map · rig sun share)`, so a tree's shadow edge crosses Jak as it crosses the grass. The rig's sun share (kits keep their lights), lamps and local lights are untouched. The heightfield march stays behind option 33 = 0 |
| Canopy sun patches | baked crown maps | unchanged for now. Switch: the patch term `sun` becomes the map's V at the receiver (crowns cast into the map), the shade's ambient darkening and cool tint stay from the crown map |
| Backlit-leaf occlusion (CanopyVol) | leaf volume march | unchanged for now. Switch: sample the map at the leaf (`ws_sun_vis`); the self-occlusion of the crown is then exact, the volume stays for the transmission depth |
| Water glint land test (water-sun-glint branch) | heightfield march along the sun | `ws_water_vis(p)` from `world_shadow_frag.glsl`: the map's visibility at the water pixel, or -1 where no cascade covers it (keep the march there). Not changed here |
| Swamp rain cover | heightfield straight up | unchanged: rain falls vertically, it is not sun visibility |

## Options (pc-set-shadow-option, values in millionths)

| # | Meaning | Default |
|---|---|---|
| 32 | world receivers (tfrag, tie, shrub, etie, wind tie) take the map | 1 |
| 33 | actors take the map instead of roof cover (option 20) | 1 |
| 34 | water may read the map (`ws_water_vis`) | 1 |
| 35 | map size per cascade: 1024, 2048, 4096 | the tier's (high: 4096) |
| 36 | cascades: 1-3 | the tier's (high: 3) |
| 37 | penumbra per metre of blocker distance | 0.012 |
| 38 | distance the last cascade reaches (m) | 120 |
| 39 | 32-38 back to their defaults (`sandover-settings-reapply` sends it) | |

The map is drawn only while actor shadows are on and one of 32-34 is set, so the boot state stays
vanilla. Today's look for an A/B: `(pc-set-shadow-option 32 0) (pc-set-shadow-option 33 0)`.

Scratch: `share_check.py` (not committed; σ, σ1 and the
colour estimate per vertex from `unbake/<level>-fit.npz`).

## Results (first round, bin world-sun-shadows-1)

Manifest `sandover/shots/world-sun-shadows.yaml`; frames under
`world-sun-shadows-001/{A,B}`, debug views (option 2 = 1 occlusion,
2 share) in `diag-occ`, `diag-share`, crops in `crops/`. A = play look with options 32-34 at 0
(today), B = play look (map on). Levels are 8-bit luma of the PNGs.

- No double darkening: on pixels whose baked direct share is under 0.05 (share view), B - A has a
  mean of -0.06 to -0.28 levels and a 95th percentile |difference| of 0.0-1.6 levels in all four
  shots. Baked-lit ground (share over 0.3) loses 4.7 (village1 15:00), 17.7 (beach arch) and 27.7
  (beach 17:30) levels on average.
- Village1 15:00 (Jak at the cliff edge; the caster is the cliff behind the camera, the palm's
  shadow falls elsewhere): the shadow edge crosses Jak between shins and torso and continues on
  the grass. Shins 47 -> 30, ground beside the feet 139 -> 98 (same direction, similar ratio), torso
  in the sun 43 -> 59. A's torso is darker because the roof-cover ray march finds the cliff in the
  heightfield for most of its rays and takes the sun from the whole actor; the map resolves the
  edge per pixel.
- Roof cover parity (Jak's back, mean luma): jungle tunnel noon A 29.0, B 29.0, terrain-sun-005 B
  29.4 (the map shades the tunnel floor and Jak; the floor's bake already holds no sun, so it is
  unchanged; Jak's floor shadow there is the local lights'); beach arch A 110.2, B 110.1,
  terrain-sun-005 B 105.6 (that round's frame differs from today's master by 4.6 with the same
  option; B matches today's within 0.2). Jak keeps the sun under the arch, which now casts onto the
  sand beyond him.
- 2x crops: grass and rock contact shadows on sand with alpha-cut blades, soft edges, no acne, no
  gap at contact. Thin dotted dark dashes on village1's lit grass (crops/v1-dashes*.png): fixed in
  revision 002, see below.
- Cost: the pass's timer query reads 2.2-3.3 ms GPU at 2048 x 3 cascades for 46k-430k triangles,
  far above the triangle estimate, so the cost is not geometry; for the optimization pass (clears,
  state, per-draw texture binds, caching the far cascade).

## Fix pass (second round, bin world-sun-shadows-2)

Same cameras and arms plus jungle-tunnel-1830; B now runs at the high tier (4096 x 3).

- Dashes: one alpha-tested tfrag card (`vil1-jng-leafyground-hang-2x1`) sampled with repeat where
  the colour pass clamps; the UV-border bleed made a sub-texel strip. Casters now use the draw's
  clamp mode. Dash detector on the lit grass, B over A: 5 dashes in 001, 0 in 002.
- No double darkening: baked-shadow pixels |B - A| p95 0.9-1.5 levels in all five shots.
- Roof cover: jungle tunnel noon A 29.2, B 29.0, roof cover 29.4; beach arch A 107.2, B 107.1, roof cover
  105.6; jungle tunnel 18:30 A 22.6, B 23.9, roof cover 22.3.
- Village1 edge across Jak and the grass: shins 46.9 to 30.1, ground beside the feet 138.7 to 97.6.
- The cascade band blends into the next cascade only where that one holds the point; wind ties
  take the shadow strength; the roof cover is dropped only when the map drew for actors.

