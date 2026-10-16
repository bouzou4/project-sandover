---
title: Black cliffs at hut-roof dusk were fire canyon drawn with village1's palette weights; haze density midpoint
date: 2026-09-25
status: verified (captures, inspect picks, offline haze fit)
sha: branch fix/black-cliff-2
---
## Answer (≤5 lines)
The black rock faces behind the village1 waterfall are fire canyon's TIE (`fcn-rockwall`), a
neighbour in vista mode. When gk could not read `sandover/tools/data/mood/<level>.json` (a
copied project root without `sandover/`, as the #179 review and the haze round used), the
vista renderer fell back to the viewer's palette weights. At 18:00 those put weight 1.0 on slot
4, which is black in fire canyon's palette. The mood tables are now compiled into gk.

## Evidence (hut-roof dusk, `vista haze`, one capture each)
| run | setup | black |
|---|---|---|
| r1 | master shaders, pr198 gk, mood JSON present | no |
| r2 | the #194 branch gk, same root | no |
| r3 | 3 instances in parallel (VRAM and load-timing check) | no |
| r4 | as r1 with `sandover/tools/data/mood` moved away | yes, same shape as haze-001 |
| r5 | as r4 with inspect picks on the darkened pixels | fire canyon tie, slot 4 luma 0, weight 1.00 |

Inspect line (r5): `tie firecanyon geom 0 tree 0 draw 3 | tex fcn-rockwall ... class stone |
slots luma*w 0:54x0.00 1:191x0.00 2:144x0.00 3:60x0.00 4:0x1.00 5:0x0.00 ...`. Fire canyon has
its own `*firecanyon-palette-interp-table*`; village1, beach, jungle and training share
village1's, which is why only fire canyon geometry went black. Load timing is ruled out: the
loader only returns a level after its textures are uploaded, and r3 was clean.

Every older black capture came from a copied root: the #179 review's `master-dc89-root` and the
haze round's `p4-haze-root` hold `game`, `goal_src`, `out` and so on but no `sandover/`. Captures
from worktrees or the main checkout (landmarks revision 2, parity193, bisect-cliff) were clean,
which is why #186 looked intermittent.

## Fix
`sandover-mood-tables` also writes `game/graphics/opengl_renderer/NeighboursMood.inc`, and
`neighbours::MoodTable` reads that table instead of the JSON. A test fails when the committed
table differs from the generator's output. The JSON stays for lightbench. Off path: MoodTable
runs only in neighbour mode with own mood on, so vista off is unchanged (no parity run).

## Haze midpoint (task from the maintainer: fog on neighbours too weak)
Far band at hut-roof dusk: rows 700-830, pixels that differ from the stand-in capture by more
than 10 and are not black (luma > 8). Luma is Rec. 709 on the PNG values.
| capture | mean luma | rms |
|---|---|---|
| vanilla stand-ins (ab-001 A = landmarks2 A) | 112.7 | 26.1 |
| haze 0.6 / km (clean master capture, r1) | 98.0 | 23.7 |
| midpoint target | 105.4 | |
| haze 0.85 / km (r3) | 106.2 | 22.9 |

The density came from an offline fit before any capture: per pixel, the haze-001 A (no haze)
and B (haze 0.6) pair gives the haze colour (177, 168, 200) and the haze amount, which inverts
to a distance; re-evaluating the curve predicted 99.4 at 0.8 and 102.3 at 0.9 on that pair's
mask, so 0.85 with the cap kept at 0.7. Misty island and geyser rock keep their overrides
(`haze_scale = 0`).

## Follow-up: basin and misty island (human review of landmarks3)
- Basin: the building seen from hut-roof was village1's own stand-in
  (`v1-lores-rolling-precursor-2`, 1.7 km; inspect pick), which the unified fog leaves much
  clearer than the vanilla fog did. A per-neighbour entry cannot reach a viewer stand-in, so the
  `haze` preset now sets `standin_levels = true` (vista option 12: rolling and village2 load
  for real and their stand-ins hide) and fogs rolling with `density_scale = 10`,
  `tint = (0.83, 0.82, 0.94)`. Fit on the building body (x 1015-1100, y 640-700): vanilla mean
  luma 145.3, real basin before 114.0, after 140.7. The tint aims the fully fogged colour at the
  stand-in's (150, 140, 183) under the haze.
- Rock village (village2), also real under option 12: `density_scale = 16`,
  `tint = (0.79, 0.79, 1.01)`. On its structure pixels at hut-roof dusk (x 1440-1540,
  y 630-720, sky excluded) mean luma 126.1 -> 140.0 and rms 13.5 -> 11.2 against the stand-in's
  144.2 (a flat fogged silhouette, rms 1.9). x4 first moved it only to 132.8, so its effective
  optical depth per unit scale is about 0.08; the remaining rms comes from small dark parts.
- Misty: `density_scale` 6 -> 16, tint (0.9, 0.87, 0.8) -> (0.95, 0.93, 0.88), haze still off.
  Island contrast to the sea/sky behind it (mean |luma - background| over the island mask,
  box x 400-1100, y 660-840): noon stand-in 13.6, before 15.5, after 7.1; dusk stand-in 10.0,
  before 17.1, after 6.6. At dusk the spires and the arch on the right still show as a faint
  silhouette (they sit higher, where the fog's height falloff thins it).
