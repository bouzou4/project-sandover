---
title: Do per-texture derived maps work in the engine in place of the procedural detail?
date: 2026-09-25
issue: none
status: inferred
sha: c1daa16a7 (base; branch pbr-maps-engine)
---
## Answer (≤5 lines)
Yes. Every shaded path binds the texture's own normal/roughness map on unit 17 per draw, and
the detail now follows the painted carving and grain instead of a class-wide noise. At the
default strengths (`presets/pbr.toml`, 0.4-0.6) the change is subtle, and smaller than the
procedural look's. Cost: about 200-290 MB VRAM per level at the derived (4x pack) size; no
frame-time change measurable on one run. Off by default; the off path is unchanged.

## Evidence
Captures (2560x1440, MSAA 4, SSAO `contact`, bloom and actor shadows on):
`$SANDOVER_DATA/shots/pbr-maps/{off,m05,m10,m20,proc}/`, manifest `sandover/shots/pbr-maps.yaml`.
Share of pixels changed by more than
8/255 against shading off:

| Shot | maps 0.5 | maps 1.0 | maps 2.0 | procedural |
|---|---|---|---|---|
| village1-oracle noon | 12.6% | 11.5% | 15.6% | 16.4% |
| jungle temple top noon | 5.1% | 5.1% | 5.1% | 5.6% |
| beach-start noon | 4.2% | 4.5% | 4.8% | 8.2% |
| village1-hut dusk | 4.1% | 5.1% | 5.5% | 8.5% |

Most of the change at any strength is the highlight, which is gated by sun share; the global
detail scale moves little (temple top: mean difference 0.04/255 between 0.5 and 2.0).
Normal view (`pc-set-pbr-param 4 2`) at the temple top shows the carved maze on the floor
and the stone rim grain; the foliage canopy is noisy in that view but not in the shaded frame.

Loads (log lines `materials: <level> maps ...`): village1 474 of 667 textures have maps,
201 MB; common textures 177, 32 MB; beach 499, 285 MB; jungle 497, 215 MB. Decode on the
loader thread took 0.2 s for village1 and 12-23 s for jungle and beach while three other game
instances and a build ran (memory was tight); upload 50-290 ms on the render thread.

Frame time (`pc-set-pbr-param 10 1`, whole-frame GPU timer, village1 oracle, last 240-frame
window after settling): 5.5 ms off, 4.9-5.5 ms with maps. Not solo: three other instances
held slots, so this only says the maps cost is below the noise.

## Parity
Not run for this PR (repo rule: parity is a milestone check). With the toggle off every new
branch sits behind the `pbr_on` uniform, the maps are never decoded or uploaded, and unit 17
is never bound; the shader edits only change code inside `pbr_background`, `pbr_generic` and
`pbr_merc`, which run only with `pbr_on != 0`.

## Implications for the design
- The class still owns the look (band shape, tint, bronze precursor); maps add where it lands.
- VRAM needs a format or size decision before a play build: BC5/BC4 or `[maps] max_size`
  (`kb/perf-backlog.md`).
- The generic camera-space light frame is still unverified on a live generic draw.
