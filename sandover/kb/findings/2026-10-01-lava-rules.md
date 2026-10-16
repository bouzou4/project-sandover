---
title: Lava rules (glow, underlight) — what the data says and how the branch uses it
date: 2026-10-01
issue: none
status: verified (collision, volumes, texel key); inferred (look, until the round)
sha: 811f54d28 (base; branch lava-rules)
---
## Answer (≤5 lines)
The collision mesh holds lava only where the designers meant "lava floor": village3 (10,038 m², the
lake and its edges), lavatube (287,048 m²), firecanyon (24,390 m², 3 m below the visible lava) and
ogre (194 m²: the arena lake is the `ogre-lava` actor, not collision). `hotcoals` is hot ground, not
an emitter (firecanyon: the cracked floor plates; lavatube: metal walkways), so it is left out. The
blackbody key `R - max(0.6 G, B)` separates molten from crust cleanly on every lava texture.

## Evidence
- Collision lava/hotcoals per level, read from `out/jak1/fr3/<level>.fr3` (CollisionMesh, Jak 1
  pat material `(pat >> 6) & 0x3f`, 12 lava, 11 hotcoals); render texture nearest each collision
  triangle from `<level>-background.glb`:
  | level | lava (m², y) | over render texture | hotcoals (m²) | over render texture |
  |---|---|---|---|---|
  | firecanyon | 24,390, y 27.0 (visible lava plane 30.0) | crackedfloor, fcn-coal, bridge, rockwall (mixed: it sits 3 m under) | 57,801 | fcn-crackedfloor 64%, -side 19% |
  | lavatube | 287,048, y -51..76 | 31% nothing within 3 m, lava-rockwall 15%, lava-floor/-blend 11% | 60,143 | lava-metal-plate-01 27%, metal planks |
  | village3 | 10,038, y 35.7-60.4 | vil3-lava-bottom 49%, crackedfloor 20%, rockwall-lower 15%, lava-top 11% | 0 | |
  | ogre | 194, y 31 | ogr-rockwall-small | 0 | |
- Lava `water-anim` types: the three `water-vol-method-22` that set `(water-flag lava)`
  (ogre-obs.gc:1022, village3-obs.gc:70, lavatube-obs.gc:1022). Volumes from the entities:
  ogre-lava x 148.7..276.3, z -1993.8..-1794.5, surface 29.0 m; villagec-lava 1011.6..1248.6,
  -3614.4..-3323.7, 36.0 m; lavatube-lava 16 x 16 m at -45 m. `water-height` is absolute.
- Vanilla lava light on characters: `light-group 0 dir2`, direction (0, -1, 0), colour (1, 0.364, 0)
  (firecanyon, lavatube: `update-mood-lava`, mood.gc:586-591) or (1, 0.38, 0) (village3
  mood.gc:1419-1428, ogre mood.gc:1506-1520, faded by distance and, in ogre, off during a flash),
  level `0.875 + 0.125 sin(lava-time)`.
- Hot texels at full resolution (`decompiler_out/jak1/textures`, key thresholds 0.3 / 0.55 as in
  the shader): in the PR description.

- The field as the engine builds it (goalc-test `LavaField.RealLevels` on the fr3 files): the slab
  roof is the first horizontal collision more than 0.5 m over the nearest lava.
  | level | field | lava m² (collision / volume) | roofed lava m² | lava at two heights m² | bake lava share |
  |---|---|---|---|---|---|
  | firecanyon | 492x1024 at 1.02 m | 24,434 / 0 | 7,530 (the road plates over the buried lava) | 105 | 0.351 (reflection only) |
  | lavatube | 1024x826 at 1.56 m | 243,461 / 0 | 112,082 (tunnel ceilings, walkways) | 19,977 (stacked tubes) | 0.000 |
  | village3 | 285x357 at 1 m | 5,211 / 36,882 | 5,809 (scaffold decks, overhangs) | 189 | 0.034 |
  | ogre | 176x248 at 1 m | 204 / 17,679 | 266 (walkway) | 0 | 0.000 |

## Implications for the design
- The field is collision lava (facing up) plus the actor volumes minus ground that reaches the
  volume surface; without the volume ogre would get nothing.
- Firecanyon's bake holds lava light (palette slots 4-7, 18-21% in the unbake report); the engine
  derives "reflection only" from the level's lava-hued palette share at load, no level list.

## Follow-ups
- F8 (firecanyon emitter misfires) stays out: fcn-bridge and precursor metal sit on top of
  collision lava in places (the collision lava is 3 m under the visible plane), which is one
  reason a bake-driven emitter score calls them emitters. `fcn-rockwall-bttm` (14% of the level)
  is lava over rock with a clean key (hot 16%), a texel-level emitter, not a whole-texture one.
- Lava haze can take the same field when the maintainer wants it.
