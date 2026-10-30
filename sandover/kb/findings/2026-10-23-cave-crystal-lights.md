---
title: Cave blue crystals give light; no world sun map in levels without sky
date: 2026-10-23
issue: play run 20261023-1042 entries 0020, 0021, 0022
status: verified (bundle re-shoots)
sha: 6d6ef3974 (base)
---
## Answer (≤5 lines)
1. The white-blue wall crystals of maincave and robocave (`cv-bluecrystal`, 47 tie clusters) had
   world-table rows in the census but no vanilla measurement, so the bake left them out. Measured
   on three vanilla frames they sit at the SDR ceiling (97-99% of their pixels), which the existing
   prop rule (`props.glows_in_vanilla`, ceiling branch) turns into light. Same class, colour rule,
   radius and gain as the snow crystals.
2. The "strange lighting glitches" (0021 dark stripes under a crystal cluster, 0022 hard-edged lit
   and dark slabs on the wall) were the world sun shadow map running in a level that has no sky.
   It now runs only in levels with sky.

## Evidence
### Crystal measurement (glowcheck, maincave, existing vanilla frames)
| Frame ($SANDOVER_DATA/rounds/lights) | contrast | saturation | ceiling own / around |
|---|---|---|---|
| crystals-005/C/maincave-cv-bluecrystal | 2.36 | 0.37 | 0.97 / 0.07 |
| crystals-006/C/maincave-cv-bluecrystal | 1.89 | 0.34 | 0.99 / 0.31 |
| crystals-007/C/maincave-bluecrystal | 6.68 | 0.31 | 0.98 / 0.00 |

Medians: ceiling 0.98 own against 0.07 around, so the ceiling branch (>= 0.5 and twice the
surroundings') passes; the contrast branch alone would not (pale: saturation 0.31-0.37). No
robocave vanilla frame shows one; it is the same texture. The play-run vanilla frames
(`runs/20260926-1011-play/vanilla`) are not used (another projection, `2026-09-30-prop-glow-check.md`).
The rebake adds 47 rows (maincave 24, robocave 23), class glow, crystal bit, 7 m, colour from the
texture (0.61, 0.84, 1.0); nothing else in `WorldLights.inc` moves.

### What the world map was doing in a cave
`ActorShadows::prepass` picks the shadow light: the sun on the disc's arc (`sun_disc_direction`)
or the moon; both need `heightfield_level_has_sky`. In maincave and robocave neither exists, so
`world_strength` stayed at -1 and the map took "its own fade on the palette sun": the direction
`unbake_sun_summary` fitted to the bake. In a cave that fit is the directional part of the cave's
baked light, here almost straight down (gk log: `sun from maincave (-0.05 1.00 0.00), share 0.73`).
The map then drew every tfrag, tie and shrub along that direction and the receivers removed their
fitted "direct sun share" (`1 - strength x max(sigma - V x sigma1, 0)`) wherever the map saw a
blocker. How much a pixel lost depends on its fitted share (n.L against a near-vertical light, so
bulges and ledges lose most) and on the blockers the map holds above it, which gives arbitrary,
hard-edged patterns that have nothing to do with the cave's light: under the crystal cluster in
0021 (tie, drawn into the map) the wall lost its whole baked share in long vertical columns, and
the crystals' own faces, shadowed by each other, were drawn purple-grey; in 0022 the stalactites
and ledges cut hard slabs out of the wall. The edges are hard because the blockers are close to
the wall (penumbra 0.012 per metre). Why some walls under the cave roof kept their light was not
chased: in a level without a sun the map is wrong either way.

The fix: `map_strength` eases to 0 over 0.3 s (`kFadeS`, the cave-dark factor's fade) when the viewer's level has no sky (`LevelSky.inc`, from level-info).
`WorldShadows::prepass` then draws no cascade, so every consumer (background receivers, actors per
pixel, water) loses it; actors fall back to the terrain cover, which has no heightfield in a
sky-less level, i.e. what master did before the world map. The actor sun map is untouched.

### Proof (bundle cameras, player's settings, `$SANDOVER_DATA/shots/cave-crystal-lights/`)
| Test | master | fix |
|---|---|---|
| 0021 stripe: luma of the column under the cluster / the wall beside it (should be about 1) | 0.12 | 1.08 (glow props off), 1.22 (on) |
| 0021 mean abs luma difference against master with the world map off | 23.0 | 3.3 (glow props off) |
| 0022 the same | 8.1 | 1.2 (glow props off) |
| Light the crystals add, mean luma within 150 px of the crystal (fix minus fix with glow props off) | 0 (no rows) | 0020 +3.4, 0021 +51.6, 0022 +43.9 |
| Crystal mesh, 90th percentile luma in a 120 px box (vanilla 245.8 / 231.5) | 0020 158.2, 0021 77.1, 0022 186.3 | 0020 222.6, 0021 255.0, 0022 218.4 |

The world-map test fails on the old frames (23.0, 8.1, stripe 0.12) and passes on the new ones.
0020's crystal is 27 m from the camera behind mushrooms, so its pool is small there; 0021 and 0022
show the pool on the wall around the cluster. Sheets: `proof-sheet-00{20,21,22}.png`.

Capture note: `(pc-set-emissive-lights 19 0)` (glow props off) survives a settings reset and the
rig groups shots by level, so an "off" arm reaches every later shot of the instance. The first
0022 fix frame was taken after it and is kept aside in `proof-fix/contaminated/`; the 0022 fix
frame in the table comes from a clean instance (`order/0022-a`).

### Cost
In the caves the world map was cached (`0.00 cascades redrawn per frame`, pass about 0 ms); the
cost was the receivers' lookups. At the 0022 camera (2560x1440 render, FSR, shared GPU, not a solo
run) the `tfrag` group took 0.77-0.86 ms per 60-frame window on master and 0.63-0.66 ms on the fix
(merged build): about 0.15 ms less. The 47 crystal rows take glow slots the props already had.

## Implications for the design
- Other levels without sky lose the world map too: jungleb, sunken, lavatube, citadel, darkcave.
  Their fitted "sun" is the same kind of artefact; darkcave keeps its crystal mechanism
  (`cavecrystal`, gate 12) unchanged.
- A crystal on a wall lights it at a grazing angle; the pool reads because the light sits on the
  cluster's vertex nearest its centroid, a few tens of centimetres off the rock.

### Other levels without sky (review request)
Probe frames at the darkcave camera of `shots/darkcave-crystal-light.yaml` (darkcave-0024, unlit)
and at the darkcave-start and lavatube-start continue cameras, play look, master and fix builds
one after the other: every darkcave frame came out all zero (PNG and EXR peak 0) on master and on
the fix alike, and the last lavatube pair too, while four other gk instances shared the rig; one earlier
lavatube pair (continue camera, particles on) was valid but is dominated by smoke particles (mean
luma 24.4 master, 25.3 fix). No judgement of darkcave or lavatube is possible from these; frames
under `$SANDOVER_DATA/shots/cave-crystal-lights/skyless*`.

## Follow-ups
- Frames at a sky-less level other than the caves (darkcave, lavatube, citadel) to confirm nothing
  liked depended on the world map there; the rig gave black frames for darkcave on master too.
- `LevelSky.inc` has sunken `false` and sunkenb `true` (from level-info), so the world map now
  runs in sunkenb and not in sunken, two halves of the same ruin. Check which flag is right before
  relying on it for shadows there.
