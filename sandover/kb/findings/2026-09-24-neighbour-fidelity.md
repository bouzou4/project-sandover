---
title: Can every adjacent level be drawn as real geometry instead of stand-ins? (neighbour fidelity spike)
date: 2026-09-24
issue: neighbour fidelity (design-inputs, vista verdict)
status: verified (renders, own moods, cost, parity) / refuted (neighbour waterfalls and windmills)
sha: d76e41401 + branch p4/neighbour-fidelity
---
## Answer (≤5 lines)
Partly. With `(pc-set-vista "neighbours")` every level adjacent to the game's two levels stays
resident in the C++ loader (up to 6 levels at village1-hut) and its whole background (tfrag, tie,
shrub, trans/dirt/ice/water tfrag) is drawn in place, each with its own mood palette for the hour,
under unified fog, with the viewer's `medres-*` stand-ins removed. Distant land is no longer lit
black and no longer mixes detail with stand-ins. But the beach/jungle waterfalls and windmills are
actors, not background, so they disappear with the stand-ins instead of becoming real.

## Evidence (path:line@sha / URL, short excerpts)
Code (branch `p4/neighbour-fidelity`):
- `game/graphics/opengl_renderer/Neighbours.{h,cpp}`: adjacency table (vista facts, 20 pairs), resident
  set shared between game and render threads, viewer stand-in table, `medres-<level>` matcher,
  `MoodTable` (mood JSON -> `update-mood-palette`/`update-mood-quick` weights -> packed itimes).
- `pipelines/opengl.cpp` `gl_set_levels`: in neighbour mode the want list is the game's levels
  plus their neighbours; `Loader::set_max_levels` raises eviction to resident + 1. GOAL's level
  slots are untouched.
- `VistaRenderer`: one tfrag/alpha-tfrag/tie/shrub renderer set per neighbour; opaque at the
  bucket-30 hook, alpha trees after `TFRAG_ICE_LEVEL1`. Levels the game loads but does not display
  (beach at `village1-hut`, `:disp1 #f`) are drawn too.
- `Merc2::handle_pc_model` skips `medres-<level>` models of drawn neighbours; the neighbours' own
  `lores`/`medres` draws are skipped inside them.
- Options 5-10 of `pc-set-vista-option` (`kmachine.cpp`, `kernel-defs.gc`); runbook
  `kb/howto/vista.md`.

Mood: the renderer also evaluates the viewer's own table and compares it with the itimes the game
sent. At the staged hours 12/18/23 (effects off) the log reads `viewer mood drift 0/64` on every
shot (64/64 only on the frame the rig jumps the clock). The C++ port matches GOAL exactly.

Captures (`$SANDOVER_DATA/shots/neighbours/`, manifest `sandover/shots/neighbours.yaml`, 2560x1440
MSAA 4, Jak hidden, effects/wind/particles off; `off2/` vista off, `nb/` neighbours + unified fog
150 m / 0.15 per km / H 800 m). Gallery: `$SANDOVER_DATA/shots/neighbours/gallery.html`.
- hut-roof (110 m over the village1 hut): jungle, firecanyon, training and misty are real. The
  medres-jungle cliff with its baked waterfall is replaced by the real jungle geometry, which has
  no waterfall (the jungle waterfall is `jungle-water`/particle actors).
- high (450 m): firecanyon's real lava river and misty's structures; dusk reads coherent.
- beach-v1 (beach shore toward village1): village1 is a game level there and was already real;
  misty island is now real and lit by misty's own mood at night instead of black.
- hut (continue camera): the far jungle towers are real; the waterfall is gone.
- The beach windmill (`windmill-one`) and the waterfall particles are actors of beach; beach is
  never displayed from village1-hut, so they are not drawn.

Cost, indicative only (GPU timestamps, 240-frame windows, 2560x1440 MSAA 4, solo, no other gk;
`cost.json`):

| view | buckets off | buckets neighbours | vista-draw | vista-alpha | VRAM delta |
|---|---|---|---|---|---|
| hut-roof | 0.90 | 2.76 | 1.80 | 0.02 | +404 MiB (first load) |
| hut | 1.62 | 3.80 | 2.09 | 0.02 | +128 MiB |
| high | 0.42 | 1.69 | 1.32 | 0.02 | +143 MiB |
| beach | 1.68 | 1.72 | 0.55 | 0.01 | +128 MiB |

VRAM from `nvidia-smi` (whole GPU, so other processes add noise); the 404 MiB is the first
neighbour set loading. No LOD, frustum-only culling, all neighbours at LOD 0.

Parity: `sandover-parity check --set auto-v1` with this build, vista off: PASS, 56/56 shots under
the calibrated threshold (FLIP mean <= 0.0110).

## Implications for the design
- Background-only neighbours are coherent in fog and palette, but the stand-ins carried baked
  animated features (waterfalls, windmills, water) that only actors provide. Real neighbours
  need their static decorative actors drawn too (merc/generic without GOAL processes, or a
  limited GOAL-side "display, no logic" state for neighbours).
- Loader: with two instances running, one gk hit `bad allocation` in `loader_thread` while
  loading 6 levels (host memory pressure). A single instance ran all 12 shots. A memory budget
  and a residency cap are needed before this is default.
- Black gaps where no level exists (left of village1 in the high view) remain; they are there
  with the vista off too.

## Follow-ups
- Decide how neighbour actors are drawn (see PR "Decision needed").
- Per-neighbour LOD and a coarse visibility test (cost 0.5-2.1 ms today).
- Extend the stand-in table beyond village1 (only `v1-lores-trees` and the merc `medres-*` are
  handled; `vil1-medres-grass` stays as ground skirt).
