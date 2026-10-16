---
title: Moving sun, first pass - sidecar, vertex order, experiments
date: 2026-09-23
issue: "#130, #131, #132, #133"
status: verified (vertex order, parity); experiments await the maintainer
---
## Answer (<=5 lines)
The per-vertex sidecar maps 1:1 onto engine vertices: every tfrag and tie tree of the five
village1-table levels matches on vertex count and position digest (`unbake: village1 7/7 trees
match`). The moving-sun delta renders in `tfrag3_sun` behind a Jak 1 toggle; toggle off passes
the parity gate (56/56, max FLIP 0.0029), toggle on with the sun at the fitted slots is
pixel-identical to vanilla (FLIP 0.00000 on 10 day shots). Captioned sheets for the moving sun
and the three night variants are ready for the maintainer; no look decision is made here.

## Evidence
- Vertex order: the glb exporter writes `unpacked.vertices` as the tree's vertex buffer
  (`decompiler/level_extractor/fr3_to_gltf.cpp`, `add_tfrag`, `add_tie`), the loader uploads the
  same array (`loader/LoaderStages.cpp`), draws use `glDrawElements` without a base vertex, so
  `gl_VertexID` is the vertex index. Checked at load by count + float32 position digest for
  every tree (log lines in `$SANDOVER_DATA/logs/slot*/`), and visually by the debug view that
  replaces the bake with the fitted model: `sheets/mapping-*.png` (FLIP 0.13 vs vanilla, the
  fit residual, no scrambling).
- The writer also proves the fit arrays are the glb's non-wind vertex subset in tree order
  (exact float equality of all 677,734 village1 positions).
- Sidecar sizes: village1 9.4 MB, beach 7.3, jungle 7.5, training 7.4, finalboss 4.4.
  Mean confidence q over fitted tfrag/tie vertices 0.80-0.91 (q from the worst day-slot
  residual, 1 at <= 8 u8, 0 at >= 32 u8; shrubs and k > 3.99 get q = 0).
- Parity, toggle off: `sandover-parity check` on auto-v1, 56/56 pass, mean FLIP 0.00026, max
  0.0029, threshold 0.0116 (`parity/runs/20260923-135233-check-auto-v1/`).
- Parity, toggle on, identity mode (every slot targets its own fitted sun): FLIP 0.00000 mean
  and max over the 10 day shots (`experiments/sun-experiments/day/identity`).
- Effect size, FLIP vs vanilla: arc sun over hours 8-16, mean 0.069 (max 0.129; 0 at 12:00,
  where one slot is live alone); noon offsets -30/-15/+15/+30 deg: 0.34/0.19/0.16/0.26;
  night table moon 0.073, ambient only 0.116. Offline on village1 the arc delta is p90
  6-11 u8 at mid-blend hours.

## Sheets (all under `$SANDOVER_DATA/experiments/sun-experiments/sheets/`)
| Sheet | Columns |
|---|---|
| `day-village1-hut.png`, `day-beach-start.png` (+ `-crop`) | vanilla, moving sun, identity (must equal vanilla); rows 8, 10, 12, 14, 16 h |
| `noon-offset-*.png` | vanilla noon, sun 30/15 deg earlier, 15/30 deg later along the fitted arc |
| `night-village1-hut.png`, `night-beach-start.png`, `night-jungle-start.png` | fitted moon (the bake), table moon, ambient only; hour 23 |
| `mapping-*.png` | vanilla, fitted model instead of the bake, sidecar normals |

## Notes and deviations from the design
- Delta is per slot, not on blended `d(t)`: `sum_s w_s [target_s - model_s]` with the palette
  weights `w_s` recovered from `itimes`. Identity is exact by construction.
- "Follow the arc" takes the arc position from the live day-slot weights, so it matches the
  game's hour table (slots full at 7, 9, 12, 15, 18 h) with no GOAL hook; the sun direction is
  slerped between fitted slots and `v` interpolated between the two nearest slots.
- Record layout is 16 B per vertex (RGBA32UI, route A of the unbake plan), a and k as unorm16, not the
  12 B record the plan assumed; one `<level>.unbake` per level next to the fr3. The plan's record layout should be updated.
- "Fitted moon" at night is the bake itself: the fit reproduces it, so the only background
  change comes from the table and ambient-only variants. Characters keep the table moon in all
  three (merc dir0 untouched; #70).
- Not covered: envmapped tie base draws, wind ties, shrubs, lowres tfrag, LOD > 0, merc.
- Normals for SSAO: record.y carries an area-weighted normal smoothed over position-welded
  vertices for every statically drawn tfrag/tie vertex (fit orientation where fitted, winding
  otherwise, flag bits 8-9 of record.w). The hook is documented in `background/Unbake.h`;
  SSAO is not wired to it.
- Cost: not measured (no solo GPU window in this pass). The shader adds 7 slot evaluations and
  one buffer fetch per vertex on tfrag/tie draws only when the toggle is on.

## Follow-ups
- DECISION (human): does the moving sun look good enough to keep (review outcome 1); which
  night variant (decision 4).
- Merc dir0 follow the controller (#70); envmapped ties; cost measurement solo.
