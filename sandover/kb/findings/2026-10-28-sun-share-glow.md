---
title: The world sun map keeps the bake's glow (Mountain Pass crystals light the ground again by day)
date: 2026-10-28
issue: Mountain Pass by day, ground around the red crystals grey-brown instead of red
status: verified (rig, three Mountain Pass cameras, two world-sun-shadows-002 cameras, one night frame, village3 and firecanyon at noon under the level gate)
sha: 8b9a84887 (base)
---
## Answer (5 lines)
1. Option 32 off brings the ground back (mid camera: ground 150.5, 80.1, 60.7 with the map off,
   56.7, 42.5, 45.1 with it on), so the world sun map alone removed the glow. The sky hue (option
   30) only tinted what was left; the material pass adds light, it does not remove it.
2. The unbake fit is not to blame: around the crystals it fits a sun share of 0. Those vertices
   hold the same colour in all four palette slots, day and night alike (mid tfrag 208, 107, 107 in
   every slot), which ambient plus sun cannot reproduce, so their fit confidence q is 0.1-0.3 and
   the shader blends to its fallback estimate, `1 - ws_amb / lum(colour)`: everything above the
   level's median ambient counts as sun. Shrubs (the mushrooms, the pebbles) always use that
   estimate. It read 30-47 % of the glow-lit colour as sun; the canyon floor is in the map's
   shadow, so that share was removed.
3. Fix: per palette entry, `PaletteGlow.cpp` measures the light the bake holds in every slot and
   the shaders take it out of the sun share (estimate minus the glow's share of the live colour,
   fitted share times one minus it). It applies to tfrag, tie, envmapped tie, wind tie and shrub.

## Why not the chroma rule
The first idea was to keep only the sun-coloured (achromatic under a white sun) part of a vertex
colour as sun. Measured on the fit data it is not enough: the crystal glow baked into the ground
ties is nearly white (mid ties 132, 122, 122 against 66, 64, 64 on the floor 20 m away), and on
the red tfrag floor the achromatic capacity above the median ambient is still 33 % of the colour.
Full occlusion would leave the ground at 67-78 % of vanilla. Excluding vertices within glow-prop
light radii was the other option; it needs a list of props and a radius per level, and glow the
bake put elsewhere (lava, lamps lit at every hour) would still be read as sun.

## What the rule is
A palette entry holds a vertex's light in each slot. The sun part changes with the slot (its
direction, its colour, nothing at night in ogre), the ambient part follows the slot's ambient, and
a glow is the same in every slot. Over the level's fitted day and night slots (sidecar header),
with the fitted ambient A_s and sun L_s:

    C_s = a A_s + sigma_s L_s + e,   a, sigma_s >= 0 (sigma free per slot), e >= 0 rgb constant

Because sigma is free per slot, the sun can explain any sun-coloured light in any slot, so e takes
only light that neither the ambient nor a sun of that slot's colour can. Gates, all global:
- the model without e must miss by more than 12 u8 rms (the fit's q runs from 8 to 32);
- e counts above 16 u8 of luminance;
- the level must have a sunless fitted slot (see "Level gate" below);
- the entry must be steady: every channel's range over the slots under 10 % of its brightest slot
  for full weight, falling to 0 at 20 %. Without this the free emission term soaked up the
  dawn-slot floor of sunlit village1 grass (the fit's global colours miss those entries by 30 u8),
  and its shadows came out lighter;
- clipped samples (254 and up) are left out; a channel clipped in every slot counts from its
  dimmest slot above the ambient, and an entry clipped everywhere has no glow.

Share of vertices with glow on the fitted data without the level gate below: ogre 19 %,
firecanyon 17 % (lava), village3 6 % (its mood rewrites the lights every frame, so its palette is
nearly constant), village2 1.1 %, misty 0.9 %, swamp 0.7 %, the others under 0.4 %. The gate keeps
ogre, rolling, swamp and misty.

## What the GPU does now
- The loader thread, right after it reads the sidecar, solves every tree palette of the level
  (`palette_glow_prepare`: non-negative least squares over the 15 supports of (a, e), alternating
  with sigma, 60 rounds, only for entries that pass the steadiness and residual gates; ogre about
  60 ms). It runs whatever option 32 says, so switching the map on mid-level costs no solve.
- At the first draw of a tree under the map, `palette_glow_setup_tree` uploads one R8 texel per
  entry (zero past the palette) to a 1D texture on unit 59 (`ws_glow`, bound at link).
- Vertex shaders: `ws_glow_share(time_of_day_index, colour)` = 2 x texel / lum(colour), clamped,
  0 unless option 32 is on and the tree has a glow texture (`ws_glow_on`). The estimate becomes
  `1 - ws_amb / lum - glow`; the fitted shares are multiplied by `1 - glow`.
- `fitted_share * (1 - glow)` (`tfrag3_sun.vert`) is an approximation: the fit's sun part was
  solved without an emission term, so on a vertex lit by both, the sun part may already hold some
  of the glow; scaling by the glow's share of the colour removes at most that much, never adds.
- Option 32 off: no GL call, the shader term is 0: the master path.

## Proof (`$SANDOVER_DATA/shots/sun-share-chroma/`, play look, `measure.json`, `sheet/`)
Ground = background pixels within 5 m below the crystal light (the crystal sheet mask), sRGB mean.

| Camera (12:00) | vanilla | master | branch | map off | branch red vs vanilla |
|---|---|---|---|---|---|
| pass-mid-crystal | 159, 82, 61 | 57, 42, 45 | 150, 80, 61 | 150, 80, 61 | -5.7 % |
| pass-end-crystals | 103, 34, 25 | 42, 20, 18 | 85, 30, 22 | 90, 31, 23 | -18.0 % |
| pass-ledge-crystal | 98, 39, 29 | 41, 23, 22 | 87, 36, 27 | 87, 36, 27 | -11.4 % |

The end camera's ground is 12.6 % under vanilla with the map off (the play look); the map now
takes 6 % there (a rock face in the mask that varies between slots, so the steadiness gate leaves
it to the estimate), 0.2 % at mid and 0.1 % at ledge.

The test that fails on the old frames: ground red within 15 % of the map-off frame at the three
cameras (master -62 %, -54 %, -52 %; branch -0.2 %, -6.2 %, -0.1 %).

Shadow references (master shot twice for the rig noise; "stable" pixels moved at most 1 level
between the two master runs): mean luma change of the darkest 30 % of stable pixels +0.02
(village1-edge-1500) and +0.02 (beach-arch-1315), of the brightest 30 % -0.12 and -0.16 levels.
Pixels moving more than 2 levels: 27 % and 23 % of the frame against 26 % and 32 % between the two
master runs (Jak, water, grass). Night (pass-mid-crystal-night, 23:00): shadow +0.03, lit -0.04
levels, 9 % of pixels over 2 levels against 7 % master noise.

## Level gate: a sunless slot (review)
The split only applies on levels whose fitted day and night slots include one with no sun (sun
luminance under 5 % of the brightest slot's, `kGlowSunless`): ogre, rolling, swamp and misty pass,
village1, beach, snow, firecanyon and village3 do not, and get no glow texture, so the shaders run
the master path there. Where every slot has a sun, sigma can absorb a constant light in some slots
but not in others, and what the emission term takes depends on how the slot suns' colours differ:
over village3's day slots a constant grey entry came out as 98 u8 of glow and a cyan one as 89,
while sun-coloured and tinted ones gave 0 (`test_palette_glow.cpp`). With a sunless slot, the
light held in that slot is unambiguous.

Before the gate, one noon shot each (lava-rules cameras, play look, clocks pinned) lightened
village3's shadowed lava-lit glow geometry by +6.6 levels (master 58.7, map off 76.5) and
firecanyon's by +1.1; a nearest-vertex split could not show that sunlit rock wrongly given glow
stayed put, so the gate was added. With it (`$SANDOVER_DATA/shots/sun-share-chroma/v3fc-after5`
against two master runs, v3fc-before and v3fc-before2): village3 shadowed pixels -0.32 levels
against -0.37 master to master; firecanyon +1.69 against +1.24, inside its lava-pulse noise
(41 % against 37 % of pixels over 2 levels). Pass-mid-crystal is bit-identical to the ungated
build.

Lost: the lava light the bake holds on lava-lit ground in firecanyon and village3 is again read
as sun where the map shadows it. Getting it back needs a lava-aware rule (the lava field or the
lava effect slots as the light source), not this split.

## Cost
Not profiled (perf after approval). Per vertex one 1D fetch; per tree one uniform and one bind
while the map is on, one 8 KB upload at its first draw. The solve runs on the loader thread.

## Follow-ups
- A vertex lit by both a glow and the sun varies between slots and keeps the estimate (the end
  camera's rock face). Separating the two needs the normal (the fit's per-vertex model with an
  emission term) and records for shrubs.
- The crystals themselves still lose about 13 % under the map on master until the crystal prop row
  makes their texture self-lit.
