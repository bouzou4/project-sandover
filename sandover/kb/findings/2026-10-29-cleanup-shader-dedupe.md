---
title: Shader snippet dedupe through the include table (behaviour-preserving cleanup)
date: 2026-10-29
issue: none (refactor: shader snippet table)
status: verified (expanded source) / inconclusive at pixel level (rig noise, see below)
sha: bce89f0a7 merged + branch cleanup/shader-dedupe
---
## Answer (≤5 lines)
`Shader.cpp` now splices snippets from one ordered table, and twelve snippets that were copied
across 2 to 8 shaders live in shared `.glsl` includes (shaders: 129 lines added, 323 removed).
The source each program compiles is unchanged: 332 stage files (Jak 1 and Jak 2) are 320
byte-identical and 12 differ by one or two blank lines, 0 differ otherwise; the C++ splice agrees with
the script that proves it (166 of 166 Jak 1 files, base and head). The rig could not
show pixel equality at 0 on most shots, because two runs of the same build already differ.

## Evidence
What the engine does. `Shader::Shader` turns a program's `.vert`/`.frag` (and tessellation) text
into the strings it hands to `glShaderSource`: it replaces tokens such as `PBR_BG_FRAG` with
the text of a snippet file, then substitutes `HEIGHT_SCALE` and `SCISSOR_*`. Before, that was 20
`include` calls (first use only) and three hand-written loops (every use). Now one table
`kSnippets` (`token, file, stages, every_use`) in the same order; `LEVEL_HEIGHT_GLSL` stays last
so earlier snippets can use it.

New includes, each replacing text at the position it had (shaders keep their own uniforms and
outputs in the same order):

| Snippet | Used by |
|---|---|
| `atmo_uniforms_vert` (`atmo`, `haze`, `haze_f`), `atmo_fog_vert`, `atmo_combine_vert` | tfrag3, tfrag3_sun, shrub, tie_wind, etie, etie_base (combine also merc2, emerc) |
| `atmo_fog_merc_vert` (uniforms and the `vec3` variant) | merc2, emerc |
| `em_value_vert` | etie, etie_base, tfrag3, tfrag3_sun |
| `emissive_mask_frag` | bloom_bright, contact_shadow, emissive_light, ssao_composite |
| `normal_decode_frag` | emissive_light, ssao_ao, ssao_view |
| `pbr_albedo_grad_frag` | etie_base, shrub, tfrag3, tie_wind |
| `fog_tint_frag` | etie_base, shrub, tfrag3 |
| `iw_column_frag` | generic, merc2 |
| `blur_vec` | motion_blur, motion_blur_tiles |
| `merc_clip_axis_vert` | merc2, emerc |

Left alone: the ice refraction block (`merc2.frag`, `pbr_bg_frag.glsl`). The two copies read
different inputs (`pbr_mpos`/`vtx_st`/`textureGrad(tex_T0)` against `pbr_wpos`/`uv`/
`pbr_albedo_grad`), so sharing them means a function with arguments, which changes the compiled
text. `emissive_tiles.frag`'s `emissive` is a different function and stays.

Gate 1, expanded source (`sandover/tools/sandover/shader_expand.py`, test
`tests/test_shader_expand.py`): reads the table from `Shader.cpp` (the pre-table order for the
base tree), expands every program for Jak 1 and Jak 2, and compares with blank lines and
indentation ignored.
- base vs head: 332 files, 320 byte-identical, 12 whitespace-only, 0 different, 0 missing.
- The 12 are `etie`, `etie_base`, `shrub`, `tfrag3`, `tfrag3_sun+tfrag3`, `tie_wind` vertex
  stages, x2 versions: a snippet file ends in a newline and the token sits on a line of its own,
  so one extra empty line appears (`etie.vert` gets two, expanded lines 101 and 115). No declaration moved.
- Native cross-check (a throwaway patch, not committed, dumping the spliced source before jitter injection; `scene_jitter::inject` is not modelled, it is deterministic on identical input):
  166 Jak 1 files per build; base native equals the script's base 166/166, head native equals
  the script's head 166/166, base native vs head native 160 identical + 6 whitespace-only.
Gate 2, build: `gk` builds clean; every shader links (the engine asserts on a failed program).
Across all rig runs the gk logs hold no shader compile or link line, and the warning/error
lines are the same set for base and head (one pre-existing GL_INVALID_ENUM on startup).
Gate 3, tests: `pytest` in `sandover/tools`: 907 passed, 2 failed, 7 skipped on the branch
before the merges; the two failures (`test_play_guard` run_shots) fail the same on master
(they pick up a vanilla capture from the data folder). After merging master, one test fails
(the export test, two master notes name export-excluded tools), also from master.

Gate 4, pixels. Rig: `--bin` staged copies of base (master at the branch point) and head,
`--parallel 1 --slot 1` (the slot matters: runs on slot 2 differ from slot 1 by themselves),
MSAA path (no `--play-look`: its temporal AA gives 60% differing pixels between two identical
runs, with jitter off still 22%), clocks and wave pinned with the `ssao-over-fog` stage hook and
`pc-set-water 90` where the manifest has none, recipes from `howto/` per manifest: vista-fog
(unified fog + neighbours), emissive and tile-lights (`--preset ssao=deep --preset emissive=default`),
ssao-over-fog (fix on), pbr-classes (`pc-set-pbr-param`), ice-depth and inland-water as written.
Base 2-3 runs (A1-A3), head 1-2 (B, B2); HDR target on for vista-fog, emissive, tile-lights.
Results over 108 shot comparisons, `pixdiff --threshold 0` semantics:
- 20 shots: a head run is pixel-identical to a base run (0 differing pixels).
- 62 shots: the closest head run differs from a base run by no more than the base runs differ
  from each other.
- 26 shots over the base spread. Three SDR shots
  have three agreeing base runs but two head runs that differ from each other by 71-83k
  pixels (Jak's blink phase on `misty-shore-jak` and `jungle-tower-noon`, two lake-dither states
  on `rolling-lake-A`; their head differences from base are 47 / 940 / 81,075 px, base floor 0),
  which is rig nondeterminism, not a regression. The other over-spread shots are not all
  floor-sized. Real ratios (head difference against base floor, pixels):
  - sdr2 `ice-crystal-crate-B2`: floor 57,586, head 1,888,512 (33x).
  - icepin `ice-crystal-crate-vanilla`: 410,993 against 1,892,419 (4.6x).
  - `snow-control-0004-vanilla`: 1.54M against 3.05M.
  - HDR `village1-hut-night-particles`: 3,918 against 20,174 (5x).
  - HDR `darkcave-start`: 23,576 against 71,865 (3x).
  These are single-head-run shots, so there is no head-vs-head floor to compare with. The
  `ice-depth` manifest runs a `warmup-play-look` shot first, which applies the play look; the
  manifest does not show whether its temporal AA stays on for the later shots, and the vanilla
  arms issue no reset, so a TAA state carried over is a plausible source of the large ice
  differences. Not proven. merc2's expanded text is byte-identical across base and head, so
  its shots cannot differ through this change; the text-identity gate (Gate 1) is the proof,
  not these pixels.
- The EXR differences are of the size of the noisy floors (0.2 to 7 absolute, on bright
  emitters), so the "under 1 output level after the encode" test is not decidable either.
So the pixel gate shows no shader failing or looking wrong; it does not prove equality, the
expanded-source gate does. A re-run that meets "0 where the floor is 0" needs a deterministic
rig (frozen Jak and particles, no temporal AA); noted for the rig. Data: the run folders
under `$SANDOVER_DATA/shots/cleanup-shader-dedupe/`.

## Implications for the design
New fork snippets go in the table and a `.glsl` file instead of a copy; a snippet used twice in
one stage needs `every_use` or an include guard. Tokens must not contain one another (the test
checks).

## Follow-ups
- Ice refraction block: share it only with a pixel-level gate that can see 1-level changes.
