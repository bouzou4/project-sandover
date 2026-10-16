---
title: Can we get a sampleable depth texture mid-frame in the Jak 1 renderer, and what does it cost?
date: 2026-09-23
issue: #12
status: verified
sha: 749d11b49 (branch e4/ssao-plumbing)
---
## Answer (≤5 lines)
Yes. After bucket 30 (`SHRUB_GENERIC_LEVEL1`) every draw of both level slots is opaque or alpha-tested, and at that point we copy the main FBO depth into our own texture. Characters are **not** in that depth: Jak and most actors are drawn in the pris buckets (48-56), after the alpha buckets. With MSAA, avoid the `glBlitFramebuffer` resolve. It is implementation-defined (sample 0 at 4x, something else at 8x on this driver), and it costs an average of 0.5-3.6 ms with 10-40 ms spikes. Copy MSAA to an MSAA texture instead (0.21-0.26 ms at 4x, 0.41-0.45 ms at 8x at 1440p) and read samples in the shader. At MSAA 1 the copy costs 0.055 ms. With everything off, the output is pixel-identical.

## Jak 1 bucket order, with the opaque boundary marked
Source: `game/graphics/opengl_renderer/buckets.h:5-75`, renderers in `OpenGLRenderer.cpp:653-860` (`init_bucket_renderers_jak1`).

| # | Bucket | Renderer | Depth behaviour |
|---|---|---|---|
| 3 | SKY_DRAW | sky | no geometry depth |
| 4 | OCEAN_MID_AND_FAR | ocean-mid-far | |
| 5-11 | TFRAG_TEX/TFRAG/TIE/MERC_TFRAG/GENERIC_TFRAG **L0** | tfrag, tie(+envmap), merc, generic | opaque/alpha-test, writes z |
| 12-18 | same for **L1** | | opaque/alpha-test, writes z |
| 19-24 | SHRUB_TEX/NORMAL/GENERIC **L0** (21-23 empty) | shrub, generic | alpha-test, writes z |
| 25-30 | SHRUB_TEX/NORMAL/GENERIC **L1** (27-29 empty) | shrub, generic | alpha-test, writes z |
| **── capture point 0: after 30 ──** | | | |
| 31-36 | ALPHA_TEX / TFRAG_TRANS0+SKY_BLEND / DIRT / ICE **L0** | tfrag trans/dirt/ice | blended. Trans writes z where alpha >= 0x7e (afail FB_ONLY), dirt never writes z, ice always writes z (`extract_tfrag.cpp:1860-1888`) |
| 38-43 | same for **L1** | | same |
| 45-46 | MERC_AFTER_ALPHA, GENERIC_ALPHA | merc, generic | per-draw blend |
| 47 | SHADOW | shadow | stencil |
| 48-53 | PRIS_TEX/MERC_PRIS/GENERIC_PRIS L0, L1 | merc, generic | mostly opaque characters |
| 54-56 | MERC_EYES_AFTER_PRIS, MERC_AFTER_PRIS, GENERIC_PRIS | eyes, merc, generic | **Jak is here** (visible in the p1 shot, missing from p0) |
| **── capture point 1: after 56 (optional) ──** | | | |
| 57-62 | WATER_TEX/MERC_WATER/GENERIC_WATER L0, L1 | | blended |
| 63 | OCEAN_NEAR | | blended |
| 64 | DEPTH_CUE | | post |
| 65-66 | PRE_SPRITE_TEX, SPRITE | sprites/particles | blended |
| 67-69 | DEBUG, DEBUG_NO_ZBUF, SUBTITLE | | |

The upstream collision hook (`bucket_id == 31 - 1`) sits at the same boundary. In Jak 1 the boundary is not level0-only: both levels' shrubs come before `ALPHA_TEX_LEVEL0`. This corrects fact #9.

## What was built
- `SsaoDepth` (`game/graphics/opengl_renderer/SsaoDepth.{h,cpp}`) is called from `dispatch_buckets_jak1` after the chosen bucket. It has two modes:
  - mode 0: a `glBlitFramebuffer` resolve into a single-sample `GL_DEPTH24_STENCIL8` texture.
  - mode 1 (default): an MSAA→MSAA copy into a `GL_TEXTURE_2D_MULTISAMPLE` depth texture, and the shader uses `texelFetch(..., 0)`.
  - Scissor is saved and restored, and the draw FBO is rebound afterwards.
- A debug view draws linearized depth full-screen at the end of the buckets, using the same mapping as `make_new_cam_mat`: `z = m32 / (pzw*(2d-1) - sz)`. `SharedRenderState` gains `camera_persp[4]` for this, filled from the tfrag PC data.
- `GpuTimer`: a ring of `GL_TIMESTAMP` query pairs, read 4 frames late, which logs `SSAO-TIMING ...` averages every 240 frames. The profiler had no GPU timers.
- One-shot MSAA probe: it reads back resolved minus {sample 0, min, max, mean} and logs `SSAO-MSAA ...`.
- Toggles, all default off and all Jak 1 only:
  - ImGui: Renderer Debug → "SSAO Depth".
  - GOAL: `(pc-set-ssao-debug sym)`, `(pc-set-ssao-depth-capture sym)`, and `(pc-set-ssao-option which val)`, where which is 0 = capture point, 1 = msaa mode, 2 = gpu timing, 3 = run the MSAA probe.
  - The kernel symbols are registered only when `g_game_version == Jak1`.

## Evidence
Rig session: village1-hut, jungle-start (village1 + jungle both `active`), and beach-start (village1 + beach both `active`), each at noon, paused, with gk rendering at 2560x1440.

**MSAA probe** (resolved vs per-sample values):

| spot | samples | resolved == sample 0 | pixels whose samples differ | of those, matching none of s0/min/max/mean |
|---|---|---|---|---|
| village1-hut | 4 | 3686400/3686400 | 2741292 | 0 |
| village1-hut | 8 | 1295770/3686400 (max diff 0.012) | 2769406 | 2085433 |
| jungle-start | 4 | all | 3539200 | 0 |
| jungle-start | 8 | 350906 (max diff 0.059) | 3542976 | 2722905 |
| beach-start | 4 | all | 3223371 | 0 |
| beach-start | 8 | 531742 (max diff 0.0073) | 3228663 | 2685253 |

"Samples differ" is not just silhouettes: most sloped surfaces have sub-pixel depth variation. At 8x the driver returns a value that is not any single sample, the min, the max or the mean. It is implementation-defined, as the GL spec allows.

**GPU cost** (average over 240 frames, ms, 2560x1440). "buckets" is the timestamp span of the whole bucket dispatch; it includes CPU-submission bubbles, so it is noisy (off vs off-again differs by up to 0.6 ms).

| spot | MSAA | buckets off | buckets off-again | capture resolve (avg/min/max) | capture MS copy (avg/min/max) | capture after-pris, resolve |
|---|---|---|---|---|---|---|
| village1-hut | 1 | 5.11 | 5.35 | 0.054 / 0.048 / 0.062 | 0.055 / 0.050 / 0.062 | 0.069 |
| village1-hut | 4 | 2.04 | 1.94 | 2.90 / 0.053 / 37.4 | 0.229 / 0.180 / 0.75 | 0.99 |
| village1-hut | 8 | 2.79 | 2.72 | 0.50 / 0.093 / 10.3 | 0.451 / 0.335 / 0.86 | 0.39 |
| jungle-start | 1 | 2.17 | 2.77 | 0.055 / 0.053 / 0.065 | 0.056 / 0.053 / 0.068 | 0.069 |
| jungle-start | 4 | 2.10 | 2.17 | 2.04 / 0.055 / 16.6 | 0.264 / 0.180 / 0.70 | 0.86 |
| jungle-start | 8 | 2.81 | 3.03 | 2.51 / 0.090 / 19.8 | 0.410 / 0.338 / 1.03 | 0.72 |
| beach-start | 1 | 1.33 | 1.56 | 0.057 / 0.047 / 0.53 | 0.060 / 0.048 / 0.43 | 0.059 |
| beach-start | 4 | 1.42 | 1.27 | 3.56 / 0.048 / 23.8 | 0.211 / 0.173 / 0.82 | 0.33 |
| beach-start | 8 | 1.82 | 1.66 | 2.20 / 0.089 / 12.8 | 0.419 / 0.317 / 1.09 | 0.38 |

With the MSAA resolve, whole-dispatch time rises by 1.5-3.7 ms. The likely cause is that the multisampled depth buffer has to be decompressed mid-frame (inferred, not confirmed). With the MS copy, the rise is 0.3-0.7 ms. The debug view adds a full-screen pass on top.

**Parity** (PNG screenshots at 2560x1440, MSAA 8, same paused frame, via `pc-screen-shot`):
- off vs capture-on (debug off): 0 px differ at village1-hut and beach-start. At jungle-start, 2 px differ by 1 level, which is below the off-vs-off noise (beach-start off-a vs off-b: 20 px by 1).
- With the toggles off, the code path does no GL calls (one branch per bucket), so the vanilla frame is unchanged by construction.
- Our build vs the master build at village1-hut: different sessions are not pixel-reproducible (known rig limitation), so that pair is not a parity measure.
- Shots are in `$SANDOVER_DATA/shots/e4/`: `<spot>-{off-a,off-b,capture-on-nodebug,depth-p0,depth-p1,depth-p0-mscopy}.png`, plus `baseline-master-village1-hut.png`.

**Other observations**
- The `[screenshot] OpenGL error 0x502 ... required buffer is missing` also appears in the rig runs with the master build. It is pre-existing, not ours.
- RenderDoc was not used: the timestamp queries were sufficient, and the rig is unattended.

## Implications for the design
- **Injection point.** Capture after bucket 30 gives background-only depth: all tfrag/tie/shrub of both levels plus merc/generic drawn with tfrag textures. Capture after 56 adds characters, but by then trans/ice tfrag and alpha merc have already written depth on their opaque-ish pixels. AO applied at 30 cannot darken around Jak, nor Jak himself. Whether that is acceptable, or whether AO has to be composited after 56, is a design decision (DECISION NEEDED for E-SSAO).
- **MSAA.** Use the MS copy and resolve in our own shader (sample 0, or min/max for conservative edges). Do not rely on the blit resolve. At MSAA 1 both paths cost the same, ~0.055 ms at 1440p.
- **Normals.** SSAO needs normals reconstructed from depth, because tfrag vertices have no normals (fact #5). Derivative-based reconstruction (`dFdx`/`dFdy` of view position) or a 4-tap best-pair reconstruction is needed. The linearization parameters are now on `SharedRenderState` (`camera_persp`, `camera_fog`, `camera_hvdf_off`).
- **Where AO goes.** It has to be applied to the colour target before or during the alpha buckets (e.g. a multiply pass right after capture point 0), or else stored and applied later with a mask.

## Risks
- `camera_persp` comes from the first tfrag PC-data block of the frame. Frames with no tfrag (e.g. menus) fall back to raw depth.
- At MSAA > 1, `depth_tex()` (single-sample) is empty in MS-copy mode. The next pass needs a small sample-0/min resolve pass into an R32F or depth texture.
- The timings are from a single machine and driver, at borderless 1440p; the game's own config renders at 4K with MSAA 1 (roughly 2.25x the pixels).
- The debug view overwrites everything drawn after it, including subtitles and debug text.

## Follow-ups
- A shader-side depth resolve pass (MS → single-sample min-depth) and a half-res linear-depth buffer.
- Decide on the AO composite point (after 30 vs after 56 with a mask).
- Normals-from-depth prototype, run offline on captured depth.
