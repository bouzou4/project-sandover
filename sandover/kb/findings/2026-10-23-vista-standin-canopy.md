---
title: The jungle temple-top forest turned into bare green hills under the vista
date: 2026-10-23
issue: ""
status: verified
sha: fix/vista-jungle-forest on master 6d6ef3974
---
## Answer (≤5 lines)
The "forest" between the jungle, the precursor basin and fire canyon is a village1 stand-in,
`v1-lores-trees` (a low-res TIE canopy). The vista hid it whenever fire canyon drew as a
neighbour, but it does not depict fire canyon or any other level, so hiding it left village1's
bare ground skirt (`vil1-medres-grass`). A stand-in is now hidden only if the stand-in census puts
it on real geometry (median distance under 15 m); the canopy (72 m) always draws, as in vanilla.

## Symptom
Play run 20261023-1042, entry 0010 (jungle, hour 19, on the temple top looking south-west): the
basin and valley skyline read well, the forest floor in front of them is flat, smooth green
hills with brown canyon strips across them. Vanilla shows a bumpy treetop canopy there.

## Mechanism
- Stand-ins are cheap copies of far places that a level carries so its skyline is not empty.
  With neighbour levels drawn for real, a stand-in of a drawn neighbour would double that
  geometry, so `Neighbours.cpp` `kStandins` lists the stand-ins to hide while their level draws.
- The list was filled from the stand-in census (`$SANDOVER_DATA/vista/standins.csv`), which
  names, for each stand-in, the nearest real level's box. For `v1-lores-trees` that box is fire
  canyon, so it was listed as "fire canyon's stand-in". The same census also gives the median
  distance from each stand-in vertex to the nearest real geometry of any level, and for the
  canopy that is 72 m: only 1-2 % of its vertices lie within 10 m of any level (fire canyon
  median 117 m, jungle 229 m). It is horizon dressing over the ground skirt, not a copy of a
  level.
- From the temple top, village1 and jungle are the game's levels and fire canyon is a vista
  neighbour, so the canopy was filtered out of village1's TIE draw every frame. What remains is
  the ground skirt it sits on, plus fire canyon's real canyon walls showing through it.
- Fog, lighting and LOD were ruled out: over the field the vanilla and current frames agree in
  luminance (mid field 45.1 vs 48.1, far field 39.8 vs 43.6, basin peaks 78.2 vs 79.2, Rec.709
  luma of the 8-bit PNG) and mean colour within about 6/255. Only geometry was missing.

## Fix
`kStandins` stays the list of candidates and carries each row's census median distance; a row
is hidden only if that distance is under 15 m. The threshold sits in the gap of the census:
every stand-in that sits on its level (rolling precursor 0.4-4.1 m, village2 wall and canvas
4.3 / 7.8 m, village2 wood 7.1 m, finalboss snow 7.5 / 10.2 m) is under 10.2 m, and the next row
is a ground skirt at 18.7 m. The listed village2 and rolling stand-ins (at most 7.8 m) keep
about 2x margin and the canopy (72.2 m) about 5x. The same rule gates which levels the stand-ins
keep resident (vista option 12), so one rule decides both. `goalc-test` `VistaStandins.*`
checks that the village2 and rolling stand-ins are still hidden and that the canopy never is,
and that `standin_levels` (residency) follows the same rule. The distances are literals copied
from the census (the rolling row takes the max over its three parts); `tests/test_vista_standins.py`
compares every row with `$SANDOVER_DATA/vista/standins.csv` (prefix match, max over parts) and
fails on a mismatch or on a census row of a listed host under the threshold that the table
leaves out (one known: village1 `vil1-medres-wood`, 7.1 m, depicting village2, not yet reviewed).

## Proof
Bundle camera of entry 0010 (`sandover-capture from-snapshot`, 3840x2160, play look, vista
neighbours loaded), staged master build against this branch's gk; vanilla is the same camera
with `--no-toggles`. Data: `$SANDOVER_DATA/shots/vista-jungle-forest/`.

- Canopy coverage: in the field right of Jak (pixels 2300-3000 x 950-1250), the share of pixels
  whose blue/green ratio exceeds 0.6 (the canopy's teal against the skirt's yellow-green):

  | frame | coverage | mean luma |
  |---|---|---|
  | vanilla | 0.529 | 44.3 |
  | master | 0.231 | 49.2 |
  | this branch | 0.537 | 48.6 |

  Test: within 0.05 of vanilla. Master fails (0.30 off), the branch passes (0.008 off). The
  branch's field equals a master frame with stand-in hiding switched off (vista option 0 = 0):
  coverage 0.537 in both, luma 48.6 against 48.7.
- Stand-ins still hidden: the skyline band (pixels 1500-3400 x 560-820: basin, village2, rolling
  precursor) differs between master and the branch in 0.26 % of pixels by more than 16/255;
  switching all stand-ins back on changes 3.4 % of that band. The village2 and rolling stand-ins
  therefore stay hidden, so their geometry is not doubled.
- `goalc-test --gtest_filter=VistaStandins.*`: 3 tests pass. With master's table the canopy
  test would fail: the old row put the canopy in the filter whenever fire canyon drew.
- Hide list in the gk log at the bundle camera: master
  `'v1-lores-trees,v1-lores-vil2-canvas,v1-lores-vil2-wall,v1-lores-rolling-precursor'`, this
  branch `'v1-lores-vil2-canvas,v1-lores-vil2-wall,v1-lores-rolling-precursor'`.

## Cost
Engine timers (`pc-set-perf-timing 60`) at the bundle camera, play look, 4K, no other game
instance running at launch, boots in the order master, branch, master, branch; median of the
60-frame windows after the neighbours loaded:

| boot | GPU frame ms | tie ms | vista ms |
|---|---|---|---|
| master 1 | 20.64 | 1.42 | 2.63 |
| branch 1 | 19.11 | 1.66 | 2.49 |
| master 2 | 18.33 | 1.30 | 2.45 |
| branch 2 | dropped: another instance started mid-run (37.7) | - | - |

The canopy adds about 0.3 ms to village1's TIE group (it is a TIE draw that vanilla also
pays). The frame-time delta is inside the 2.3 ms spread between the two master boots, so it
is not resolved here.

## Follow-ups
- `vista_hide_filter` is not cleared when neighbour mode is turned off (pre-existing). Its one
  consumer, `active_vista_filter` (`background_common.cpp`), reads it only while
  `vista_neighbours` is on and otherwise uses the single-level `vista_hide`; the stand-in hide also
  requires `vista_drawn`, which is reset every frame. The stale string is never read, so it is left
  as is. The filter is computed in the vista pass after bucket 30 and read by the next frame's
  TIE draws, so it lags by one frame after a change.
- `vil1-medres-wood` (village1, 7.1 m from village2's geometry) and the other hosts' copies of
  the village2/rolling stand-ins (training, misty, village3, finalboss) are not in `kStandins`.
- The census column `nearest_real_level` is not the depicted level; any new `kStandins` row needs
  its median distance from the census, and the rule decides.
