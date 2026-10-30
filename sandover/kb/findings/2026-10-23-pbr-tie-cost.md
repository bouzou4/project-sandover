---
title: The PBR materials row's cost at the tie spots was a scratch-memory porosity table
date: 2026-10-23
issue: ""
status: verified (attribution, timing, paused A/B parity)
sha: master 25c4fa4f5 + opt/pbr-tie
---
## Answer (≤5 lines)
At 4K on the current rig the materials row cost 23-28 ms at the sea-level tie spots (8 ms at
ogre), and none of the material terms explained it: parallax, self-shadow, maps, environment,
glints and lobes together were 1-4 ms. 11-14 ms was one line: `sw_por(c)` indexed a 32-entry
`const float[]` by the draw's class, which the driver keeps in per-lane scratch memory. Selects
on the uniform class give the same values. On master with the late stencil marks the branch saves 3.7 / 6.4 /
5.3 / 1.4 ms of frame at beach-shore / village1-night / village1-hut-lamps / ogre.

## Method
`sandover-perf` (one gk, `--parallel 1`, 60-frame windows, 3840x2160, the Sandover defaults with
FSR 3.1 native), A/B/A per toggle in one boot; a gate waited for no other gk, no live slot or
build lock and an idle GPU 3D engine, and a sampler logged gk every 10 s (runs with a second gk
are discarded or flagged below). Probes, all debug, no setting:
- `(pc-set-pbr-param 41 MASK)` (`PbrParam::PROBE`, uniform `pbr_probe`): terms off, 1 parallax
  march, 2 self-shadow march, 4 normal/roughness map fetch, 8 environment, 16 glints, 32 sun and
  key lobes, 64 the whole response (`pbr_background` returns the vanilla colour), 2^25 leaf
  transmittance, 2^26 detail-normal output; 512 / 1024 / 2048 / 4096 / 8192 / 16384 return
  after Ng / after the detail normal / after the wet and bake-wrap block / before
  `pbr_respond` / after it / before the debug view; C++ in tie: 128 no highlight-material blend
  guard, 256 no emissive draw buffer. Bits 16-23 veto one `pbr_win` optimization.
- `tie-mfrags` counter: `GL_FRAGMENT_SHADER_INVOCATIONS` around every tie bucket, millions per
  frame, with `(pc-set-ssao-option 2 1)` (probe window only).
- Data: `$SANDOVER_DATA/perf/pbr-tie/` (`a1`-`a8` on master 01c4eef56, `f0` master 25c4fa4f5,
  `f1` the branch on it; `c0`, `c0b`, `c1` captures).

## Attribution (before the fix; master 01c4eef56, run a1, frame ms saved)

| measurement | beach-shore | village1-night | village1-hut-lamps | ogre |
|---|---|---|---|---|
| base frame (tie) | 43.5 (13.0) | 44.7 (13.8) | 45.1 (17.6) | 23.0 (9.3) |
| noise band | 0.5 | 0.3 | 2.2 | 1.2 |
| materials off (ceiling) | 23.2 | 27.5 | 24.2 | 8.2 |
| 64: whole response off | 20.6 | 23.2 | 21.6 | 8.3 |
| 1: parallax march | 0.1 | 0.1 | 1.0 | -0.3 |
| 2: self-shadow march | -0.1 | -0.1 | 0.6 | 0.5 |
| 4: normal/roughness map fetch | 1.2 | 1.2 | 1.1 | 0.8 |
| 8: environment lobe | -0.1 | 0.1 | 1.0 | 0.4 |
| 16: glints | 0.0 | -0.1 | 0.9 | 0.3 |
| 32: sun and key lobe math | 0.2 | 0.4 | 0.5 | 0.3 |
| 128: highlight-material blend guard (tie) | 0.0 | -0.1 | 0.1 | -0.3 |
| 256: emissive draw buffer (tie) | 4.4 | 5.0 | 4.7 | 1.5 |
| tie fragment shader runs (M/frame) | 34.4 | 31.9 | 47.2 | 43.3 |

The named terms add up to 1-4 ms of a 23-28 ms row. Bisecting `pbr_background` with early
returns (run a2, same boot; beach-shore discarded, a second gk ran): the wet/bake-wrap block cost
11.4 / 10.6 ms at village1-night / hut-lamps, the code after `pbr_respond` 7.9 / 7.3 ms, ogre
0.2 and 4.4. Neither block does work at these spots that could cost that (no rain, almost
no wet pixels, no snow, ice or lava), but both are where `sw_por(c)` is first called on their
paths: the cost is the scratch array, not the terms.

Batching: tie draws are one `glMultiDrawElements` per texture whether materials are on or off;
the material path adds per-draw uniforms and a map bind (`PbrDrawState::set`), no draw splits.
Only wind TIE is per instance, and it is so without materials too.

## Wins

| win | how | ms saved (same boot unless noted) | parity |
|---|---|---|---|
| porosity selects | `sw_por` returns selects on the uniform class; the `const float sw_porous[32]` is gone | gated A/B with the array still in the shader (run a5): 11.0 / 14.4 / 13.7 / 1.5 at beach-shore / village1-night / village1-hut-lamps / ogre; with the array removed (run a6 vs a1, separate boots): frame 43.5 -> 29.4, 44.7 -> 26.5, 45.1 -> 27.9, 23.0 -> 20.5 | identical values by construction; FLIP below |
| shore height early-out (perf-opt bit 25) | the shoreline and splash bands skip their noise where the height alone makes them exactly 0 (noise amplitude 0.5, streak at most 1) | 0.6 / 0.6 / 0.8 / 0.1 (a5); 0.2 / 0.4 / 0.1 / 0.0 on master 25c4fa4f5 (f1) | exact by construction; paused A/B below |

On master 25c4fa4f5 (late stencil marks merged), separate boots f0 / f1, same settings:

| spot | master frame | branch frame | saved | materials row master -> branch |
|---|---|---|---|---|
| beach-shore | 22.7 | 19.0 | 3.7 | 3.9 -> 1.7 |
| village1-night | 24.6 | 18.2 | 6.4 | 10.3 -> 3.8 |
| village1-hut-lamps | 24.1 | 18.8 | 5.3 | 3.6 -> -2.1 (noise band 0.6; materials off reads slower) |
| ogre | 16.8 | 15.4 | 1.4 | 4.6 -> 3.4 |

Boot-to-boot spread on this rig is large (`2026-10-22-perf-rdna4.md`), so the totals are
indicative; the same-boot evidence is the gated a3/a5 runs.

## Tried and rejected
- Parallax and self-shadow LOD (isotropic `textureLod` for the height taps, self-shadow taps by
  the march's screen length, parallax taps by the offset in pixels, one fetch shared by the
  self-shadow start and the detail map): each within noise (-0.3 to +0.4 ms) at all four spots;
  the march terms cost about 1 ms in total. Removed.
- Tie depth prepass (opaque depth-writing draws laid first with a colour-masked alpha-test
  pass, then shaded at depth `EQUAL`, no writes): 0.3-1.3 ms slower at every spot (run a7).
  Removed.

## Proof
Paused-frame A/B in one boot (`--ab-mask 0xbdffffff 0xbfffffff`, MSAA 4, no jitter, wave
clock pinned, the four spots at 3840x2160; `$SANDOVER_DATA/perf/pbr-tie/c2`). For this run only,
the staged shader kept the old `const` table behind bit 25, so A is master's behaviour (table,
no height early-out) and B the branch. FLIP (LDR) of B against A, then of A against its own
re-render A2 (the floor):

| spot | FLIP mean A/B (floor) | px > 0.1 A/B (floor) | worst 32 px block A/B (floor) | pixdiff max, share over 2 A/B (floor) |
|---|---|---|---|---|
| beach-shore | 0.0015 (0.0017) | 0.005 % (0.005 %) | 0.033 (0.043) | 16, 0.028 % (16, 0.037 %) |
| village1-night | 0.0036 (0.0038) | 0.053 % (0.064 %) | 0.137 (0.166) | 127, 0.12 % (143, 0.15 %) |
| village1-hut-lamps | 0.0062 (0.0065) | 0.124 % (0.127 %) | 0.098 (0.100) | 49, 0.41 % (50, 0.41 %) |
| ogre | 0.0046 (0.0052) | 0.062 % (0.075 %) | 0.076 (0.071) | 27, 0.19 % (25, 0.22 %) |

Every A/B figure is at or under its floor (the worst blocks sit on the same animated pixels in
both pairs), so no region changes. Bit 25 alone (run c1, the shipped shader, A without the
height early-out) gives the same picture. Captures across separate boots are not comparable
here: the hut camera settles a few pixels differently from boot to boot (FLIP 0.12 between two
master boots).

## Implications for the design
- Never index a `const` array with a run-time value in a shader on this driver, even a uniform
  one: it is placed in scratch memory and costs per fragment wherever it is live. Use selects,
  a uniform array or a texture. The other const arrays in the shaders (`world_shadow_frag.glsl`
  disks, `actor_shadow_frag.glsl` debug tint) are indexed by constant-bound loop counters;
  whether the compiler unrolls them is not verified.
- The late stencil marks cut tie fragment shader runs from 32-47 M to 10-18 M per frame at these spots (hierarchical
  depth restored), so the per-fragment cost of the material path now weighs about a third as much.

## Remaining candidates (not done)
- Leaf transmittance march (backlit foliage): 1.35 / 0.72 / 0.08 / 0.0 ms (a8). A distance LOD
  changes the integral: a look decision, not an imperceptible concession.
- Emissive draw buffer output in tie: 4.4-5.0 ms before the late stencil marks (probe 256); re-measure on master.
- Detail-normal output: 0.2-0.7 ms.
- Materials off reading slower than on at village1-hut-lamps on the new base (f1): not explained.
