# Mid ocean shading detail fades with distance (2026-10-15)

The deep-opt item "water shading LOD by distance", aimed at the water row of the profile
(ocean-mid 4.7 + ocean-near 3.2 ms at village1-waterfront-1200 after #395, ocean-mid 4.6-5.7 ms
at open-sea-low). Shipped as perf-opt bit 27 (`PERF_OPT_WATER_LOD`, default on). The bar is
"reasonable at a glance at 4K", not "no pixel changes"; the near ocean keeps the picked look
byte for byte. Measured at 3840x2160, MSAA 2 + SMAA, every Sandover row at its top tier, HDR PQ,
`wait.ok` true at every spot. The machine was shared with other runs; noted per table.

## Symptom

The mid ocean's textured pass shades every covered pixel with the full `water_shade`: world sun
map lookup, crest sparkle, texture ripple normal, hex anti-tiling, a five-wave Gerstner normal,
foam, glint and reflection, at the same cost 30 m and 3 km away.

## Where the cost is (probe)

Water option 101 switches one term of the mid's shading off (perf.md, "Water term costs"). Run
`perf/water-lod-probe-d0`: bin `opt-water-lod-1`, the fade pushed out of range, each term off over
the whole mid band. GPU ms the mid textured pass (`ocean-mid-tex`) saves with the term off:

| term | waterfront-1200 | open-sea-low | beach-whole-1730 | open-sea-high |
|---|---|---|---|---|
| mid textured pass, all on | 4.78 | 4.12 | 1.00 | 0.57 |
| world sun map lookup | 1.49 | 1.23 | 0.50 | 0.00 |
| crest sparkle (two crest filters, ~40 texture taps) | 1.06 | 0.97 | 0.09 | 0.14 |
| texture ripple normal (4 taps x hex) | 0.45 | 0.40 | 0.06 | 0.04 |
| ripple + sparkle | 1.50 | 1.32 | 0.23 | 0.18 |
| hex anti-tiling (3 taps per lookup) | 0.86 | 0.67 | 0.11 | 0.12 |
| Gerstner normal (five waves, noise, warp) | 0.52 | 0.62 | 0.05 | 0.02 |
| foam | 0.16 | 0.02 | -0.05 | 0.02 |
| reflection sun glow | 0.11 | -0.08 | 0.04 | 0.00 |
| analytic glint | 0.01 | 0.02 | -0.06 | 0.01 |
| reflection | 0.04 | 0.07 | 0.06 | 0.02 |
| everything but hex | 3.64 | 3.02 | 0.78 | 0.34 |
| the whole shading (vanilla mid) | 4.46 | 3.77 | 0.91 | 0.48 |

Group noise 0.04-0.43 ms (open-sea-low's frame noise was 19 ms: other gk ran during part of the
run; its group numbers are still consistent with the waterfront). The glint and the reflection
are nearly free; the cost is the sun map lookup and the texture work (sparkle, ripple, hex).

## Where the distance is (probe)

Option 101 = 2048 paints the mid by distance band (`shots/opt/water-lod-dist`), share of the
4K frame:

| spot | near ocean | mid < 140 m | 140-280 | 280-500 | 500-900 | 900-2000 |
|---|---|---|---|---|---|---|
| village1-waterfront (1200, 1900, 2300) | 44 % | 46 % | 0 | 0 | 0 | 0 |
| open-sea-low | 19 % | 24 % | 5.0 % | 2.0 % | 0.6 % | 0.3 % |
| beach-whole-1730 / 1820 | 21-23 % | 2.5 % | 0.7 % | 0.3 % | 0.15 % | 0.1 % |
| open-sea-high | - | - | - | - | 0.5 % | 9.7 % |

Two facts bound any distance fade:

- The near ocean draws the tiles with a corner within 136 m of the camera (`ocean-mid.gc`,
  309237600000 = 136 m squared in game units), so a near tile reaches at most
  136 + 96 x sqrt(2) = 272 m. The mid meets the near anywhere inside that, so a fade that starts
  nearer would put a detail step on the near/mid seam. The fade starts at 280 m.
- The world sun map covers 120 m of view depth (`shadow_world_range_m`), so the lookup never runs
  where the fade is; it is left as it is.

The waterfront's mid water is all within 140 m, right next to the near ocean: a distance fade
cannot take any of its 4.7 ms. Its cost is the full shader on visible, close water.

## The fade (mechanism)

One weight per mid pixel, `water_k = 1 - smoothstep(280 m, 900 m, d)`, d the horizontal
distance of the pixel's rest point from the camera (world space, so a pan moves nothing and a
dolly moves it continuously). Only the mid ocean's textured pass (bucket 3) reads it; the near
ocean, the envmap passes and the depth pass do not.

- To 280 m: everything as before (k = 1).
- 280-900 m, smoothly: the texture ripple normal and the crest sparkle are scaled by k (they
  ride on the existing texel-footprint fade, `detail`); the four short chop waves' share of the
  fragment's Gerstner normal is scaled by k, and the slope they lose goes into the glint's
  roughness (`water_slope_lost`), so the sun path keeps its energy and widens instead of
  dimming; crest foam, which comes from the chop's Jacobian, thins out with it.
- From 900 m: a flat normal from the long swell alone, the base colour and texture with hex
  anti-tiling, fog, horizon colour, sun and moon lighting, reflection and the glint. The sparkle's
  and the ripple's texture taps are skipped there, and a wave with no weight skips its phase
  noise, warp and sine.
- Unchanged everywhere: the tessellated geometry (the waves' displacement keeps the existing
  300-1200 m range falloff), fog, horizon colour, the night and moon response (sun terms are
  not scaled by k), shallow tint (already gone beyond 160 m).

Exact skips that ride on the bit (no colour change by construction): crest taps skipped where
the sparkle's factor `up * open` is 0 (sun down or the map's shadow), and a wave with zero weight
(edge 0 under floats and at the shore, or past its range) skipping its trigonometry. The
ripple taps skipped at `detail` 0 are exact too but not on the bit (always on). The game-uv
path (hex tiling off, water option 54 = 0) keeps every crest and ripple tap: its `texture()`
takes implicit derivatives, which a per-pixel branch would leave undefined.

Water options 99 and 100 move the start and the end (dev knobs, not settings).

## Proof

Look and timing ran on bin `opt-water-lod-1` (the branch before its rebase onto 39e985eb8, on
e747b7904, the crest skip synced as a shader); a same-bin bit marginal. Paused A/B, masks 0xbfffffff / 0xb7ffffff (bit 30 off as in the harness
run mask), wave clock pinned (`shots/opt/water-lod-ab27`, `pixdiff.json`, `pixdiff-floor.json`; the
harness's `ab-verdicts.json` reads fail at the far-strip spots, a parity verdict, while the bar
here is a glance at 4K); near band
= pixels the near ocean covers in a water view 20 capture of the same cameras
(`shots/opt/water-lod-masks`):

| spot | A/B max | A/B % px over 8 | floor (A vs A2) max / % over 8 | near band px | near band max |
|---|---|---|---|---|---|
| village1-waterfront-1200 | 42 | 0.001 | 0 / 0.000 | 3.64 M | 0 |
| open-sea-low | 44 | 0.026 | 0 / 0.000 | 1.54 M | 0 |
| open-sea-high | 1 | 0.000 | 0 / 0.000 | 0.00 M | 0 |
| beach-shore | 1 | 0.000 | 1 / 0.000 | 3.29 M | 0 |
| beach-whole-1730 | 30 | 0.010 | 0 / 0.000 | 1.88 M | 1 |
| beach-whole-1820 | 34 | 0.006 | 0 / 0.000 | 1.71 M | 1 |
| jungle-river-0800 | 32 | 0.024 | 0 / 0.000 | 0.00 M | 0 |
| village1-waterfront-1900 | 11 | 0.000 | 0 / 0.000 | 3.63 M | 0 |
| village1-waterfront-2300 | 7 | 0.000 | 0 / 0.000 | 3.65 M | 0 |

Every pixel over 2 lies in the far bands of the distance view, except at the waterfront, where
198 pixels (max 42) of mid water under 140 m and its edges change: the bit gates the exact skips
too, so bit on and off run differently compiled branches of one shader, and the glint's high
gain turns their last-bit differences into a few pixels. The same mechanism puts max 1 on 1 and
5 near-band pixels at the two beach-whole hours; the fade itself is 1 there.

Crops (4K, on | off | difference x8, 512 px and 256 px x3, lifted when dark):
`water-lod-1/<spot>-crop.png`, `-crop-x3.png`. Pan and dolly contact
sheets (`pan-<spot>-yaw.png`, `-dolly.png`, run `shots/opt/water-lod-pan`): five yaw steps
(-16 to +16 degrees) and a 25 m (waterfront) or 60 m (open sea) dolly in four steps; the
difference row is a thin horizon strip that moves with the camera and never steps. (open-sea-low
dolly3 drew no neighbour levels in either arm: a rig camera off the level, not the fade.)

## Timing

One in-run toggle (`perf/water-lod-toggle27`): bin `opt-water-lod-1`, bit 27 off as the
measurement against the base windows around it, the two spots where the fade can act, solo GPU,
both build lanes held from before boot. Saving with the bit on (ms; negative = slower):

| spot | frame | GPU | ocean-mid-tex (bit on) | ocean-mid-tex saved | noise |
|---|---|---|---|---|---|
| open-sea-high | 0.15 | 0.18 | 0.42 | 0.24 | 0.02 |
| open-sea-low | -0.04 (in noise) | -0.02 | 4.10 | 0.08 | 0.39 |

open-sea-high, about 10 % of its frame mid water beyond 900 m, saves a third of its mid textured
pass. open-sea-low has under 3 % of its frame past 280 m, so the fade takes 2 % of its mid pass;
the frame shows no change (0.04 ms slower with the bit on, inside 0.39 ms of noise). The village1 waterfront is not affected: all its mid water
is within 140 m (distance bands above), where k = 1; its cost is the full shading of close water
(term costs above: sun map 1.49, sparkle 1.06, hex 0.86, Gerstner 0.52, ripple 0.45 ms).

## Deferred

- The waterfront's water row: one full `water_shade` per pixel on close water. A share of it
  needs a cheaper look near the player (sun map kernel on water, crest sparkle taps, hex
  anti-tiling on the sparkle filter), which are visual judgement calls, not a distance fade.
- The exact skips could leave the bit (always on) so the A/B isolates the fade; kept on the bit
  so they can be switched off with it.
