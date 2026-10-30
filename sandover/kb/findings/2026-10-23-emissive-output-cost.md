---
title: The emissive mask output's 6-7 ms was the porosity scratch table and the stencil marks, not the output
date: 2026-10-23
issue: ""
status: verified (attribution); nothing left to fix on master 42b102d4c
sha: master 01c4eef56 and 42b102d4c, each + a local debug probe (not committed)
---
## Answer (≤5 lines)
On master 01c4eef56 switching the emissive output (draw buffer 2) off saved 6.5 / 4.8 / 6.9 ms
at beach-shore / swamp-dock1 / village1-hut-lamps. None of it was the output itself: with the
background shaders writing a constant to output 2 the output cost 0.0-0.2 ms. The cost was the
shader work that only output 2 kept live (the highlight material reads `sw_por(c)`, the
scratch-memory porosity table fixed by the porosity-table change), about 3x larger while the per-bucket stencil marks
(fixed by the late stencil-mark change) dropped hierarchical depth. On master 42b102d4c the output costs -0.5 to 0.3 ms.

## Symptom
`2026-10-23-ao-prerequisite-cost.md`, probe 16 (no emissive output): 5.9-7.1 ms at the
tie-heavy spots, 1.2-2.2 at sunken, paid whenever bloom or the tile lights are on.

## Extra work per bucket with the output on (code read, master 01c4eef56)
- Frame start (`begin_emissive`): attach buffer 2 (RG8, RGBA8 with the highlight material;
  the frame's sample count, one sample under temporal AA) and buffer 4 (R16F sprite depth, tile
  lights only); one clear of each.
- Sky (0-4): the sun disc and the mid ocean switch buffer 2 on and off around their draws.
- Background (5-30), tfrag and tie (and vista's trees): per tree and category one
  `glDrawBuffers` on and one off (`emissive_draw_buffers`), with the material one
  `glColorMaski(2, ...)` per blend change (`material_blend_guard`); tie wind one more pair per
  tree. Shrub: one `glDrawBuffers` per change of "this class writes lava E". Merc props: one per
  change of write/keep state per draw.
- After the background: generic (material clear on/off per bucket), merc characters (per draw
  state change), sprites (per draw, buffer 4 with them).
- Shaders: tfrag3, etie_base, shrub, tie_wind, merc2, generic write `emissive_out`; tfrag3 and
  etie_base write `(max(E, glow), max(glow - E, 0), pbr_hl)`, shrub `(lava, lava, pbr_hl)`.
  Their `discard` paths exist with the output off as well.
- Readers: bloom, tile lights (`EmissiveLights`, also the highlight material in b, a), contact
  shadows, the AO composite (with `ssao_emissive`).
- Not present: no MSAA emissive resolve under temporal AA, no per-draw FBO switch.

## Proof
`sandover-perf`, one gk, 60-frame windows, 3840x2160, the Sandover defaults (FSR 3.1 native,
one sample), A/B/A per probe in one boot, no other gk during the windows (the wrapper waited for
none). Probe: `(pc-set-ssao-option 5 MASK)` in a local build: 1 no output (buffer 2 never in
the draw list), 2 buffer 2 held on through buckets 0-30 (no per-tree toggles), 4 no partial
colour masks, 8 RG8 with the material, 16 buffer 2 on with a zero colour mask, 64 unchanged draw
buffer lists skipped. Frame ms saved; data `$SANDOVER_DATA/perf/emissive-out/`.

Master 01c4eef56 (run `p1`, noise bands 0.41 / 0.33 / 0.64):

| measurement | beach-shore | swamp-dock1 | village1-hut-lamps |
|---|---|---|---|
| base frame | 43.4 | 43.1 | 44.9 |
| 1: no emissive output (ceiling) | 6.45 | 4.79 | 6.94 |
| 16: buffer 2 on, colour mask zero | 6.41 | 4.81 | 6.97 |
| 2: no per-tree draw buffer toggles | -0.04 | -0.05 | -0.11 |
| 64: unchanged draw buffer lists skipped | 0.15 | -0.01 | -0.20 |
| 4: no partial colour masks | 0.06 | -0.04 | 0.13 |
| 8: RG8 instead of RGBA8 | 0.28 | 0.06 | 0.31 |
| no stencil marks | 21.19 | 19.48 | 21.83 |
| no stencil marks and no output | 22.88 | 21.63 | 24.24 |

The toggles, colour masks and format are inside noise; a zero colour mask saves as much as no
output, so it is not the writes' bandwidth. With the stencil off the output costs 1.7 / 2.2 /
2.4 ms (last two rows).

Same build, staged root whose tfrag3, etie_base and shrub write `vec4(0, 0, 0, 1)` to output 2
(run `constshader`, noise 0.28 / 0.17 / 0.45): base frame 36.9 / 38.4 / 37.9, and probe 1
saves -0.02 / 0.15 / 0.11. The cost is the computation feeding `emissive_out`, which the driver
removes when output 2 is not written. The only term there that is otherwise dead is the
highlight material's roughness `hr = pbr_hl_r * (1 - pbr_damp * sw_por(c))`, the scratch-memory
table measured at 11-14 ms at these spots (`2026-10-23-pbr-tie-cost.md`).

Master 42b102d4c (late stencil marks and porosity-table change in; run `m-base`, noise 0.11 / 0.07 / 0.16):

| measurement | beach-shore | swamp-dock1 | village1-hut-lamps |
|---|---|---|---|
| base frame | 18.8 | 21.2 | 18.6 |
| 1: no emissive output | -0.46 | 0.28 | -2.90 |
| 16: buffer 2 on, colour mask zero | 0.03 | 0.11 | -2.08 |
| `(pc-set-ssao-option 4 1)`: no stencil | 0.75 | 0.63 | 0.88 |
| no stencil and no output | 0.90 | 0.78 | 0.74 |

No change was made, so there is no image to compare: master already renders the same mask,
bloom and frame without the cost.

## Mechanism
Consistent with the driver compiling the pipeline for the bound colour outputs (inferred from
the constant-output run): with output 2 off or masked to zero the highlight material is dead code, and with it the only unconditional read of
the dynamically indexed `const` porosity array, which the compiler keeps in per-lane scratch
memory. Every background fragment that ran the material path paid for that array while output 2
was on. The per-bucket stencil marks multiplied the fragment count about 3x (late stencil marks: tie fragment
shader runs 32-47 M down to 10-18 M per frame), hence 6-7 ms with the marks, about 2 ms without.

## Implications for the design
- An "output X costs N ms" probe that switches a colour output off also removes every shader
  term only that output reads. Split it with a constant written to the output before blaming the
  output (bandwidth, export, draw buffer changes).
- Per-tree `glDrawBuffers` toggles and per-draw `glColorMaski` are free here; no batching change
  is needed for the emissive path.

## Follow-ups
- village1-hut-lamps reads 2-3 ms slower with output 2 off on master 42b102d4c (noise 0.16), as
  the porosity-table change saw with materials off at the same spot. Not explained; a pipeline variant switched at run
  time is a candidate. Not a cost of the output.
