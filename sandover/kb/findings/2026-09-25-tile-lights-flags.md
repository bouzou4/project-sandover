---
title: Emissive tile lights - fixes for the two review flags (white eco pool, tower arcs)
date: 2026-09-25
issue: #191
status: verified
sha: branch p2/tile-lights-fix (base c336038fe)
---
## Answer (≤5 lines)
The maintainer chose tile lights on (variant B) and flagged two shots. Both came from
unbounded stacking: several of the top 32 tiles sit on one small emitter, each at full power
within a metre of the receiving surface. Fixes: tiles whose glow is thin (arcs, sparks: low fill
of their bounding box) keep 15% of their power, and the summed light is soft-limited to 0.35 luma.

## Flags and causes
1. "The eco in the beach shot looked broken": the sand under the beach eco cluster became a
   flat near-white patch with a hard upper edge. The cluster spans several tiles, all ranked
   in the top 32 and placed about 0.5 m above the sand, so their power summed past 1.
2. "Weird blue electricity on the tower left of the power cell": the vent's lightning arcs
   under the housing write E; each arc tile became a light capped against the wall right
   behind it (d ≈ 0), which lit hard white patches on the wall.

## Fix
- Tile pass stores `fill` = glowing taps / taps in their bounding box. Select scales power by
  `mix(0.15, 1, smoothstep(0.2, 0.5, fill))`. A first try with plain coverage (glowing share of
  the tile) also removed the beach lamp's pool, because the lantern covers little of its tile;
  fill keeps compact emitters (lamp, eco ball, cell) at full power.
- Light pass: `sum *= 0.35 / (0.35 + luma(sum))`, so stacked lights approach 0.35 and keep hue.

## Evidence
`$SANDOVER_DATA/shots/tile-lights/{fix-off,fix-before,fix-after2}/` (same binary; before =
master's shaders). Captures `tl-002` (A before, B after). The first after
run (`fix-after`) mis-staged both shots (camera and cell state) and was discarded.
Box means over lights-off: beach eco pool +32 → +21 (p99 196 → 167), tower arc wall +27 → +19
(p99 203 → 160); visually the white patches are gone and the pools stay blue. The lamp pool
stays faint and warm. Particles on, so those numbers carry run-to-run particle noise.
No parity run: the change is inside the tile-lights pass, which is off by default.
