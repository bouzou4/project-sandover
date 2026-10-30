---
title: The temporal upscale behind one UpscalerStage, one input fill for three upscalers (behaviour-preserving cleanup)
date: 2026-10-29
issue: none (refactor: upscaler stage)
status: verified (code read, build, tests, pixel, sequence and perf gates on an emulated PSVR2 session)
sha: cleanup/upscaler-stage
---
## Answer (5 lines)
FSR 3.1, DLSS and FSR 4 now live behind `UpscalerStage` (`UpscalerStage.h/.cpp`), a member of
`OpenGLRenderer` that owns the per-eye contexts, the per-eye clocks, the frame flags, the FSR 4
eye-0 deferral and the upscaled output. The three near-identical input fills at `SPRITE` became
one `UpscaleInputs` fill; each upscaler's `Inputs` is now that struct, and each reads the same
fields with the same values as before. Captures, sequences and frame times match base within the
rig's instance-to-instance variance.

## What moved where
`OpenGLRenderer.cpp` goes from 3003 to 2859 lines (+28/-172), `OpenGLRenderer.h` -21/+4,
`OpenGLRendererXr.cpp` -9/+7. Line numbers are at `dcc6ec835`.

| Removed from the renderer | New home |
|---|---|
| constructor: `g_fsr3/dlss/fsr4_supported` probes, `D3d12Bridge` init/release, `share_programs`, `share_runtime` (`:123-132`) | `UpscalerStage::init(jak1)` |
| `render`: `m_upscaled`/`m_display_src` reset, the three frame flags, `clear_jitter` (`:1178-1185`) | `UpscalerStage::decide` (first half) |
| `render`: FSR 3.1 releases, DLSS `ready()` fallback and release, FSR 4 `poll()` fallback and releases (`:1205-1235`) | `UpscalerStage::decide` (second half) |
| `render`: `SANDOVER_XR_FSR4_SERIAL` read (`:1261-1265`) | `UpscalerStage::decide` (static, as before) |
| `render`: `m_fsr4_defer`, `m_fsr4_eye0_deferred = false` (`:1267-1270`) | `UpscalerStage::arm_fsr4_defer(eligible)`; the eligibility expression stays in `render` |
| `render`/`do_pcrtc_effects`/`hdr_scene_fbo`: `m_upscaled`, `m_display_src` reads (`:1347-1349`, `:2710-2720`, `:2878`) | `upscaled()`, `display_src()` |
| debug window: `fsr3()`, `m_dlss`, `m_fsr4_eye[0]` (`:1487-1495`) | `fsr3(slot)`, `dlss()`, `fsr4(0)` |
| `dispatch_buckets_jak1`: `temporal` (`:1822`, `:2202`) | `temporal()` |
| `dispatch_buckets_jak1`: `fsr3_dw/dh` and the three `begin_frame` calls (`:1834-1849`) | `UpscalerStage::begin_frame(rs, display_w, display_h)`; the display size is kept for the hooks and `run` |
| `capture_opaque` before `SPRITE` (`:1985-1987`) | `pre_sprite(rs, hdr)` |
| `build_reactive` after `SPRITE`, without and with motion blur (`:2064-2066`, `:2259-2261`) | `post_sprite(rs, colour)`, called from the same two places |
| the FSR 3.1, DLSS and FSR 4 hand-offs (`:2281-2361`, 80 lines) | `UpscalerStage::run(rs, prof, mv, colour, hdr)` |
| `OpenGLRendererXr.cpp`: `temporal`, eye-1 output reset, eye source, eye-0 deferred hand-back | `temporal()`, `clear_output()`, `upscaled()`/`display_src()`, `eye0_deferred()`/`finish_eye0_deferred()` |
| `OpenGLRenderer.h`: `m_upscaled`, `m_display_src`, `m_fsr3_eye[2]`, `fsr3()`, `m_fsr3_frame`, `m_fsr3_clock_eye[2]`, `fsr3_clock()`, `m_dlss`, `m_dlss_frame`, `m_fsr4_eye[2]`, `m_fsr4_defer`, `m_fsr4_eye0_deferred`, `fsr4()`, `m_fsr4_frame` | `UpscalerStage` members, in the same relative order (construction and destruction order of the contexts unchanged: FSR 3.1, clocks, DLSS, FSR 4; eye 0 before eye 1) |

Stays in the renderer: `m_display_w/h` (`setup_frame` reads it for `render_scale`), `m_stereo_cut`,
the motion-vector and AO per-eye histories, the SSAO/motion-vector releases.

## The one fill (what was unified)
| Field | FSR 3.1 (old) | DLSS (old) | FSR 4 (old) | `UpscaleInputs` (new, all three) |
|---|---|---|---|---|
| `colour` | render FBO colour tex | same | same | same |
| `depth` | `render_fb_depth_tex` | same | same | same |
| `velocity` | `mvr().velocity_texture()` | same | same | same |
| `reset` | `!mvr().frame_valid()` | same | same | same |
| `hdr` | `m_fbo_state.hdr` | same | same | same |
| `frame_ms` | eye clock `getMs()`, then `start()` | same | same | same, read and restarted inside each hand-off as before |
| `w_k0`, `w_k1` | motion-vector frame | not a field | motion-vector frame | motion-vector frame (DLSS does not read them) |
| `tan_half_x` | motion-vector frame | not a field | not a field | motion-vector frame (only FSR 3.1 reads it) |
| `tan_half_y` | motion-vector frame | not a field | motion-vector frame | motion-vector frame (DLSS does not read it) |
| `sharpness` | not a field | not a field | `fsr3_sharpness` | `fsr3_sharpness` (only FSR 4 reads it; FSR 3.1's RCAS reads the global) |

Checked by reading `Fsr3Upscaler::run`, `DlssUpscaler::run` and `Fsr4Upscaler::run`: no upscaler
reads a field it did not get before, and the extra reads in the fill (`motion_vectors::frame()`
for DLSS) have no side effect.

## Order and state kept
- Call order at `SPRITE` is unchanged: reactive capture before the bucket, mask after it (or
  after the held HUD sprites with motion blur), bloom, sun flare, velocity resolve, then the
  hand-off, FSR 3.1 then DLSS then FSR 4 as separate `if`s. Profiler children (`fsr3`, `dlss`,
  `fsr4`) and perf groups (`FSR3`, `DLSS`, `FSR4`, `e1:` per eye) are the same calls in the
  same scopes. No log line changed.
- Jitter: the same `begin_frame` calls with the same phase (`display_tick` in stereo).
- FSR 4 per-eye hand-back: eye 0 defers when the same eligibility holds, eye 1 takes eye
  0's pending targets, eye 0's output is blitted after eye 1's hand-back and copied after eye 1's
  copy. `SANDOVER_XR_FSR4_SERIAL=1` still turns it off (checked below).
- One move in time: the frame flags, `clear_jitter` and the output reset are now set after
  `setup_frame` (with the releases) instead of before it. `setup_frame` reads none of them and
  draws nothing; `clear_jitter` only sets the CPU-side jitter. The motion-vector and AO eye-1
  releases now run after the upscaler releases instead of between FSR 3.1's and DLSS's (GL
  deletions only).
- DLSS: compiled (the real path, not the stub; its pre-existing `version_str` warning is in
  both builds), not run: no NVIDIA card on the rig. Its hand-off is the old code with the
  shared fill.

## Gates
Base = `dcc6ec835` (branch point), head = `a45e8f58c`, built in the worktree with the build lanes
and staged with the same root. Data: `$SANDOVER_DATA/shots/cleanup-ups/`.

- Build: gk, goalc, goalc-test clean; the rebuilt units (`OpenGLRenderer`, `OpenGLRendererXr`,
  `UpscalerStage`, the three upscalers) add no warning. Rebuilt clean after merging master
  `b6a3c2191`.
- Tests (logs `{goalc-test,pytest}-{base,head}.log`): goalc-test 1712 of 1714 on both, the same
  two `WorldLightsGlow` failures. pytest 892 passed, 13 skipped, 1 failed on both
  (the export test, pre-existing). goalc-test ran
  holding rig slot 0: its runtime binds port 8112, and with a rig game on that port the listener
  tests connect to that game instead.
- Probe: village1-hut noon, mean 138.6 / std 51.6 on base.
- Pixels, per shot (share of pixels more than 8 apart; floor = base A1 vs base A2, head = A1 vs B,
  and B vs A2). All shots run with actors frozen (the rig default) and the wave clock pinned:

  | Set | shots | median floor | median A1-B | median B-A2 | shots where head is above floor + 0.1 pt against both bases |
  |---|---|---|---|---|---|
  | fsr3-stills, FSR 3.1 (base pair A5-A2, see below) | 8 | 1.69 % | 1.96 % (A5-B) | 1.65 % | 0 |
  | fsr3-stills, FSR 4 | 8 | 2.42 % | 2.53 % | 2.37 % | 2: `foliage-jak` 4.03 / 4.55 / 3.38, `village1-lamps-night` 0.99 / 2.91 / 2.50 |
  | tonemap-hdr (FP16 target) | 4 | 0.53 % | 0.66 % | 0.60 % | 1: `sunken-precursor-glow` 0.84 / 1.25 / 0.96 |
  | vr-synthetic mono | 4 | 0.43 % | 0.39 % | 0.03 % | 0 |
  | vr-synthetic XR (emulated PSVR2, 14 shots x main/left/right) | 42 | 0.36 % | 0.28 % | 0.25 % | 3 (`village1-hut-xr-nojak` main/left/right 0.23-0.27 / 0.47-0.53 / 0.49-0.56) |

  Particle shots are excluded as noise: `fountain-particles` is 60 % between two base runs (FSR
  3.1) and 55.8 / 72.9 / 75.1 % (FSR 4). FSR 4 `village1-lamps-night`: a third base run (A6)
  is 3.77 / 3.23 % from A1 / A2, so the base-base floor there is 0.99-3.77 %, and head sits at
  2.20-2.91 % from the three base runs: lamp flicker at night (d1 saw 3.26 / 3.99 / 1.15). HDR EXR max abs
  (scene-linear, single pixels) floor / head: beach 2.79 / 2.56, lava 0.08 / 0.24, sunken
  0.80 / 0.78, village1 night 4.64 / 4.40. Base instance A1 shot the FSR 3.1 stills at another
  camera field of view (every shot 55-90 % off both other runs, a zoomed frame), so FSR 3.1 was
  re-shot on base (A5) and the floor is A5 against A2.
- Vanilla parity (`sandover-parity check --set auto-v1`) does not test either build here: the
  vanilla library holds a frame for every auto-v1 camera and hour, so the check reuses those
  (`RIG:VANILLA-REUSE`) and renders nothing; base and head report the same 49 of 56 against the
  older golden. The vanilla path is not touched by this change (`temporal_aa` is off there and
  `decide` releases everything, as before).
- Sequence gate, no upscaler (the path the timing move touches most directly): d1's
  `seq2.steps` (`look msaa 2`) on head, emulated PSVR2, wave clock pinned. The 10 still frames
  are bit-identical (max channel difference 0 over all 9 consecutive pairs, both eyes); the
  option-7 sweep passes (per-pair mean abs within 1.4 % / 1.7 % of its median, left / right).
- FSR 3.1 with the jitter off (`pc-set-aa 4 0` after the look, one attempt, head): not
  bit-identical (max difference 85 / 52 levels, mean abs 0.023, share > 8 at most 0.001 %), so
  the history still moves with a fixed jitter; recorded, not pursued. FSR 4 has no jitter
  switch; its floor comparison below stands.
- Sequence gate with FSR (emulated PSVR2, village2 play camera, game paused, wave clock pinned, render
  scale 67 %; `pc-set-xr-fake-option 8 10`, still then option 7 sweep, FSR 3.1 then FSR 4 in one
  instance). Runs: base A1, A2, A3, A4; head B, B2, B3.
  - Frozen: not bit-identical on either build with FSR (the jitter cycles every frame, so the
    history keeps settling): share > 8 between consecutive frames at most 0.023 % (FSR 3.1) and
    0.006 % (FSR 4) on every base and head run; mean abs 0.076 / 0.10 levels on both.
  - Moving: per-pair mean abs within 1-3 % of its median on every run whose 10 frames are all
    fresh game chains. When a repeated chain (`fresh 0`, the 60 Hz game tick under 90 Hz display)
    lands inside the window, two pairs jump and the spread check fails: FSR 4 sweep in head B
    (frame 2) and B3 (frame 7) and in base A4 (frames 1, 5, 7). It happens on both builds.
  - Head vs base per frame (share > 8): FSR 3.1 still floor 0.69 %, head 0.63 / 0.60 %; FSR 4
    still floor 0.70 %, head 0.62-0.85 %; sweeps below the A1-A2 floor on 20 of 20 eye frames
    (the sweep's start phase differs per instance).
  - The held head-turn margin (not touched) rose to 17-23 deg after the FSR 4 switch in
    head B, B2, B3 and in base A4: pose age spikes during the FSR 4 warm-up (a 200 ms kernel
    compile), timing-dependent.
- `SANDOVER_XR_FSR4_SERIAL=1` (head): the serial path runs (FSR 4 still sequence, 10 frames,
  share > 8 between frames at most 0.005 %), and the `fsr4: cpu ms` lines show eye 0 handing
  back on its own (5.6-6.2 ms), where the default deferred path logs eye 0 0.00 ms and eye 1
  6.6 ms (one lock for both).
- Perf (emulated PSVR2, the play camera of the interop-lock change, 33 %, steady 5 s present window after a warm-up
  window, no other game running): FSR 4 base 19.30 ms, head 19.32 ms; FSR 3.1 base 11.11 ms,
  head 11.11 ms (90 Hz pacing; the interop-lock change measured 19.3 / 11.2 ms).

Real session not run (no SteamVR session up).
