---
title: Emissive tile lights - glow from particles and emissive surfaces on nearby floor and walls
date: 2026-09-25
issue: #191
status: verified
sha: branch p2/emissive-tile-lights (base 83156a039)
---
## Answer (≤5 lines)
A screen-space pass turns the emissive mask into up to 32 point lights per frame: 64x36 tiles
pool `ramp(E) x colour` and an E-weighted position from SSAO's depth copy; the brightest tiles
light opaque pixels within 4 m with a wrapped N.L and a `(1 - d/r)^2` window. Vents, the power
cell and lamps now put a soft coloured glow on the ground next to them. Off by default, needs
SSAO on; about 1.6 ms at 2560x1440 MSAA 4 (recorded, not tuned).

## What it does
`EmissiveLights.h`, `kb/howto/ssao.md` "Emissive tile lights". It runs after the sprite bucket
(so particles have written E), before bloom, reusing SSAO's depth copy, stencil (bit 7 clear =
opaque background and characters) and normal target. No ordering problem came up: the depth
copy and csz mips from bucket 56 are still valid at bucket 66, and the stencil is untouched
after the character buckets.

- Tile position: a particle's depth is the surface behind it, so a tile's depth is capped at
  1 m behind its nearest surface. Emission over sky alone makes no light.
  Later (fix/eco-vertex-light): emissive sprites write their own depth to a side target, so a
  tile over a particle cloud takes the cloud's depth rather than the ground seen through it.
- Power = mean tile flux x the tile's area at the light's depth, so a lamp gives the same light
  near or far. The inverse square was dropped after the first run: with it the glow stayed on
  the vent rim and never reached the floor 1-2 m away.
- No self-light: pixels whose own E is above the ramp get nothing, and a light skips pixels
  within 0.2 m of it.
- Half of the added light is tinted by the surface colour, half is the emitter colour.

Defaults (`presets/emissive.toml` `[emissive_lights]`): intensity 8, radius 4 m, falloff 2,
tile threshold 0.02. Intensity 1 was invisible, 30 blew the vent column out; 10 read as a clear
but soft glow at all three spots, 8 is the default. Candidate `strong` = 20.

## Evidence
Captures, 2560x1440 MSAA 4, 23:00, particles on, SSAO deep + bloom in both:
`$SANDOVER_DATA/shots/tile-lights/{off,on}/`, intensity sweep in `.../{b01,b03,b1,c10,c30,c30r6}`.
Capture set `tl-001` (A off, B on).

| Shot | pixels changed > 2/255 | what changes |
|---|---|---|
| jungle-top-cell-night | 11.6% | warm glow on the tower-top rim from the power cell, blue on the vent housing |
| jungle-vent-night | 11.8% | blue light on the vent bowl and the floor to its left; the pillar behind the column brightens |
| beach-lamp-night | 5.3% | warm pool at the lamp post base, blue pool around the beach eco vent |

Particles on is not reproducible run to run, so part of each count is particle noise (off vs
intensity 0.1 already changes about 6% on the jungle shots).

Shot choice: village1 has no eco vent actor (`village1-actors.json` in the entity dump has no
`ecovent`/`ventblue`; jungle has one of each, beach seven `ecovent`), so the vent shot is the
jungle one. darkcave-start has no lit crystal (2026-09-23 emissive finding), so the third shot
is the beach lamp.

## Cost (solo, one measurement)
jungle-vent-night, 2560x1440 MSAA 4, no other gk running, two 240-frame windows:
`emissive-lights-total` 1.61-1.63 ms. Likely dominated by the tile pass (2304 fragments each
reading 400 taps serially; low occupancy). A two-level reduction would be the first thing to
try in the performance pass. Memory: a full-res colour copy plus 64x36 and 32x1 RGBA32F targets.

## Limits
- Off-screen emitters give no light: a vent just outside the frame or behind the camera stops
  lighting the floor in front of Jak, and its glow pops back when it enters the view. The same
  happens when an emitter is hidden behind an occluder. Fixing that needs world-space lights
  (actor positions from the engine), not screen tiles.
- A tall particle column puts its upper tiles near the wall behind it, so the wall behind a
  vent brightens more than the floor in front of it.
- Membership of the top 32 changes as tiles cross thresholds; a small light can pop.
- Needs SSAO on; with SSAO off the pass does nothing.

## Parity
No parity run: everything is behind `pc-set-emissive-lights` (default off). With it off,
nothing new is allocated or drawn; the only shared-path edit is the sprite E write condition,
which is unchanged when the new flag is off.
