---
title: Mountain Pass red crystals give light by day
date: 2026-10-28
issue: play report "you never added lighting for the red crystals in the mountain pass"
status: verified (A/B sheet, three cameras)
sha: 627939c5c (base)
---
## Answer (5 lines)
1. The Mountain Pass is level `ogre` (`level-info.gc`; `speedruns-h.gc` names `il-ogre` "Mountain Pass IL";
   the 67 crystal clusters lie between the `ogre-race` and `ogre-end` continues, z -3150 to -3450 m).
2. The red crystals are the tie texture `ogr-cave-crystal-01`. Unlike the cave crystals (cave-crystal change, no baked E,
   prop), the census has them as statics: baked E on the night source (`e_source` 9, role `night`) only.
3. The bake turned them into lamp rows gated by the night weight (`slot` 9), so they lit nothing from
   dawn to dusk, while vanilla draws them 3x brighter than the rock at noon.
4. They are now glow-class crystal props, as `cv-bluecrystal`: 68 rows, 7 m, gain 1, texture colour
   (1.0, 0.221, 0.144), always on; the texture's 59 night statics are dropped (one light per crystal).

## Measurement (glowcheck, vanilla noon, `$SANDOVER_DATA/shots/ogre-crystals/vanilla`)
| Frame | contrast | saturation | ceiling own / around |
|---|---|---|---|
| pass-end-crystals | 3.18 | 0.42 | 0.37 / 0.00 |
| pass-mid-crystal | 3.04 | 0.41 | 0.39 / 0.04 |
| pass-ledge-crystal | 3.41 | 0.32 | 0.44 / 0.01 |

The existing prop rule rejects them: contrast passes, but the pale pink fails the saturation guard
(0.6) and the ceiling share (0.39) stays under 0.5. The saturation guard exists because a pale matte
prop can be brighter by albedo alone. A texture the level bakes non-zero E for is one the game treats
as an emitter (here at night only: the E says nothing about noon, the noon evidence is the contrast).
Rule added (`props.glows_in_vanilla(cfg, emitter)`): where the texture is also a census static with
non-zero energy on the level, the contrast branch alone suffices. The bake then drops
that texture's statics (`static of a texture lit as a prop`), so the crystal is not lit twice at night.
No other prop texture has statics, so no other level's table moves.

## Side effect in the table
Dropping 59 crystal statics frees ogre's `night_cap` (64): 23 `ogr-coal` night clusters that were
beyond the cap are now in the table (lamp, `slot` 9, night only). They all lie in the Klaww arena
(z -1835 to -1993 m), about 1.2 km from the nearest crystal.

## Proof (play look, master bin vs branch bin, `$SANDOVER_DATA/shots/ogre-crystals/sheet/`)
Ground = background pixels (not crystal) within 5 m of the crystal's light and below it, mean sRGB.
C = gain 2, shot on the branch bin with `(pc-set-emissive-lights 18 600000)` (world_glow 0.6, twice
the default); the crystals are ogre's only baked glow props, so in these frames that equals gain 2.
Sheet `sheet-A-master-B-lit-C-gain2.png`.

| Camera (noon) | A master | B gain 1 | C gain 2 |
|---|---|---|---|
| pass-end-crystals | 41.9, 19.6, 18.5 (26.1) | 47.6, 20.7, 19.2 (28.6) | 52.6, 21.7, 19.9 (30.8) |
| pass-mid-crystal | 56.7, 42.5, 45.0 (47.0) | 75.8, 46.2, 47.5 (55.2) | 92.1, 49.4, 49.5 (62.2) |
| pass-ledge-crystal | 41.2, 23.1, 22.2 (28.4) | 57.1, 26.3, 24.2 (35.3) | 70.4, 29.1, 26.1 (41.1) |

Vanilla ground there: (103, 34, 25), (159, 82, 61), (98, 39, 29).

### Night (23:00)
- Crystals, lamp class to glow class (`sheet-night-A-master-night-B-lit.png`, mid camera): ground
  169.3, 84.6, 62.9 (luma 107.5) on master, 194.1, 106.1, 83.5 (129.9) on the branch.
- The 23 coal rows (`night-klaww-arena-coal.png`, `ogre-start` continue camera, ground within 7 m of
  the coal rows): master 92.6, 34.4, 10.7 and a second master frame 88.3, 33.1, 10.5 (the lava
  animates), branch 88.8, 33.2, 10.5. Whole-frame mean absolute difference 0.15 for master vs master
  and for master vs branch: no measurable change at this camera. A first branch frame (taken on a
  retry after a hold timeout) differed by 2.45 and was not used; a re-shoot matched master.

## Crystal colour in the play look (open, not fixed here)
By day the crystals render lavender-blue (mid camera, crystal pixels: 107, 69, 86) where vanilla
shows pink (237, 134, 121). Same build, same frame, one toggle each:

| Arm | crystal RGB |
|---|---|
| play look | 107.0, 68.5, 86.1 |
| world sun map off (`pc-set-shadow-option 32 0`) | 232.1, 128.9, 115.4 |
| sky hue off (`pc-set-shadow-option 30 0`) | 132.5, 62.4, 54.4 |
| both off | 232.3, 129.0, 115.5 |
| PBR off (`pc-set-pbr-param 0 0`) | 103.2, 65.2, 83.4 |

The world sun shadow map removes the crystals' fitted direct-sun share: their bright noon vertex
colour (the glow is painted into the day palette) is fitted as sun, and the map puts the crystal in
shadow. The sky hue then tints what is left toward the ambient's blue. At night the crystal's own E
(night source) sets `shadow_receive = 1 - E` and protects it, which is why the night frames are pink.
The material class (`special`: no normal, spec or tint) plays no part. The same mechanism turned the
cave crystals purple-grey in the cave-crystal change (there the map ran in a level without sky). Fix size: not a
one-row material change; the receivers need to know the surface is self-lit by day (an emitter
flag per texture for the world map, from the glow-prop list, or day E in the sidecar for these
textures), a small engine and data change of its own.
