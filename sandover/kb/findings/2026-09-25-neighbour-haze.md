---
title: Light haze on distant land in neighbour mode, and a darker, obscured misty island
date: 2026-09-25
status: verified (captures, contrast fit) / open (the A/B pick)
sha: branch neighbour-haze
---
## Answer (≤5 lines)
The unified fog (0.15 per km from 150 m) leaves everything within a kilometre almost as crisp
as the foreground, so from the hut the jungle temple, the plain and the rock village read flat.
A haze term now adds `haze_max * (1 - exp(-haze_density * max(d - haze_start, 0)))` toward
`fog_color * haze_tint` on every background draw in neighbour mode. Defaults: 0.6 per km from
200 m, cap 0.7, tint (1.1, 1.12, 1.0). Misty island and geyser rock skip it (`haze_scale = 0`)
and get a darker, denser fog of their own instead (x6 and x7, tint below 1).

## Why every background draw, not only neighbour draws
The plain and the huts at left in the hut-temple shot are village1's own low-res
geometry (`v1-lores-*`; rock village loads for real only behind option 12), so a term on
neighbour draws alone left them crisp. The haze starts at 200 m (about village1's edge from its
centre), so near geometry is untouched; neighbours keep a per-level multiplier.

## Fit (RMS luminance contrast of a region, vanilla stand-in vs real geometry)
| region | stand-in rms | crisp rms | haze rms |
|---|---|---|---|
| hut-temple noon, jungle temple (~640 m) | 36.0 | 46.9 | 36.6 |
| hut-temple noon, plain at left | 9.1 | 31.9 | 27.7 (village1 geometry, first build: neighbours only) |
| temple-top dusk, misty island | 14.3 | 25.8 (unified) / 6.8 (intent x12) | 14.3 (x6; x5 gave 19.0) |
| beach noon, geyser rock | 26.5 | 54.8 | x7 from the sweep (x4: 38.0, x8: 26.8) |

- Target for the light haze: 70% of the way from the crisp look to the stand-in's contrast.
  The mean colour shift from crisp to stand-in at noon (+3, +17, +25) points at a lighter,
  slightly bluer colour than the noon fog (128, 180, 243): tint (1.1, 1.12, 1.0). The tint
  multiplies the mood's fog colour, so dusk and night haze follow the time of day.
- Misty: the earlier `intent` fit matched the stand-in's mean colour, which needs x12 and
  flattens the island to rms 7 (the "ghosted" dusk look). Matching contrast needs about x5-6;
  the stand-in's mean is then out of reach without a lighter fog colour, and it was asked
  for darker, so the tint goes below 1 (0.9, 0.87, 0.8). The island keeps its mass and spires
  and reads dark and obscured.
- Region stats: mask `|off - unified| > 10` in a box around the island, as in
  `2026-09-24-neighbour-fog.md`; the landmarks2 temple-top camera differs slightly from the
  neighbour-fog one, so the misty numbers on the new captures are indicative.

## Code path and cost
`first_tfrag_draw_setup` sets `haze` (density, start, cap) and `haze_color` in the unified
branch; the vertex shaders compute `haze_f` inside `if (atmo.x > 0.)`, so vista off or any
other fog mode never runs it (parity path unchanged, no parity run). Shaders: tfrag3,
tfrag3_sun, tie_wind, etie_base, etie (the additive envmap pass fades by `1 - haze_f`), shrub.
Cost: one exp per vertex, one mix per fragment and one varying; not measured.
Merc, emerc and sprites (landmark actors such as the temple rings, the waterfall) take the
same term through `vista_atmo_foreground_uniforms`, at the global density: they do not know
their level, so the misty and geyser rock `haze_scale = 0` overrides do not reach them.

## Captures
`$SANDOVER_DATA/shots/neighbour-haze/{intent,haze}/` (manifest `sandover/shots/landmarks2.yaml`,
shots hut-temple-noon, hut-roof-dusk, temple-top-dusk). Compared as
A current `intent`, B `haze`.

## Follow-ups
- Human pick; if B wins, make `haze` the default vista preset.
