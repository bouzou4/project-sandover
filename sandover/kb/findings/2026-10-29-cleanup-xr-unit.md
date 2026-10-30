---
title: XR and stereo frame path moved into its own translation unit (behaviour-preserving cleanup)
date: 2026-10-29
issue: none (refactor: XR render unit)
status: verified (static move check, build, tests, pixel and sequence gates on an emulated PSVR2 session)
sha: cleanup/xr-stereo-unit
---
## Answer (5 lines)
The stereo and OpenXR half of the frame (`dispatch_buckets_stereo`, the eye copies and crop, the
2D layer, the eye dumps and the frame sequence capture) now lives in `OpenGLRendererXr.cpp`. It is
a pure move: the 527 moved lines are byte-identical, the functions stay members of
`OpenGLRenderer`, and the only other edit gives `make_fbo` external linkage. No call order, GL state
transition or member ownership changes; the byte-identical move is the proof of no change. The
captures agree within the rig's instance-to-instance variance.

## What moved
- `ui_layer_begin`, `ui_layer_finish`, `dispatch_buckets_stereo` (with its `copy_eye` lambda),
  `copy_eye_to_xr`, `eye_crop` (still in an anonymous namespace), `save_xr_eyes`, `xr_seq_capture`,
  `copy_eye_to_xr_cropped`: `OpenGLRenderer.cpp` lines 2494-3020 at `d10288cc2`, 527 lines, into
  `game/graphics/opengl_renderer/OpenGLRendererXr.cpp` after a 16-line include header.
  `OpenGLRenderer.cpp` goes from 3530 to 3003 lines.
- Same class, new translation unit. A friend class or an interface would have meant rewriting
  every member access the moved code makes (`m_stereo_fbo`, `m_fsr4_eye[0]`,
  `m_render_state`, `fsr3()`, `tonemap_to_sdr`, `finish_screenshot`, `set_tonemap_uniforms`, ...),
  and that rewrite is the risk a pure move is meant to avoid. Splitting the definitions keeps the
  declarations, member state (`OpenGLRenderer.h:247-257`) and every access exactly as they were.
  `dispatch_buckets_stereo` still calls `dispatch_buckets` and reads the upscaler state directly.
  That coupling is what the upscaler-stage refactor (`UpscalerStage`) will cut.
- Not moved: the `SANDOVER_BG_PROBE` stage logger. Its lambda captures the locals of
  `dispatch_buckets_jak1` (`stages`, `probe`, `bad`, `draws`), so pulling it out takes a struct
  in place of a lambda capture, which is not a move. It is a background-bucket debug tool, not XR.

## Mechanical edits (the complete list)
1. `OpenGLRendererXr.cpp`: a 16-line header (one comment line, `<algorithm>`, `<cmath>`,
   `<cstring>`, `OpenGLRenderer.h`, `log.h`, `FileUtil.h`, `gfx.h`, `GlQueries.h`,
   `OpenGLRendererInternal.h`, `OpenXr.h`), then the 527 lines unchanged.
2. `make_fbo` leaves the anonymous namespace (the stereo target reallocation calls it), and the
   default argument moves to the declaration in the new `OpenGLRendererInternal.h`. The body is
   unchanged and the anonymous namespace reopens for `prime_camera_jak1`.
3. `OpenGLRenderer.cpp` includes `OpenGLRendererInternal.h`; `game/CMakeLists.txt` lists the new
   file.

Proof of the move: `git show d10288cc2:.../OpenGLRenderer.cpp | sed -n 2494,3020p` against
`tail -n +17 OpenGLRendererXr.cpp`: `diff` empty. `git diff --numstat` on `OpenGLRenderer.cpp` is +3/-530.
The 530 removed lines are the 527 moved lines, the blank line after them, the old
`namespace {` and the old `make_fbo` signature. The 3 added lines are the include, the signature
without the default argument, and the `namespace {` reopened before `prime_camera_jak1`.

## Gates
Base = `d10288cc2` (the branch point), head = the refactor commit. Both were built in the worktree
with the build lanes and staged with the same DLLs, data and root.

- Build: gk, goalc and goalc-test clean, with no warning from either renderer unit, before and
  after merging origin/master (`f3dccc779`, audio; `666b0f333`, the bridge split).
- Tests, logs in `$SANDOVER_DATA/shots/cleanup-xr/{pytest,goalc-test}-{base,head}.log`:
  - Base (`d10288cc2`): pytest 894 passed, 7 skipped, 2 failed (`test_play_guard`
    `fail_mode_starts_nothing`, `wall_cap_during_the_first_wait_boots_nothing`: the vanilla
    library on this machine holds a jungle-start noon frame, so the test shot is reused and
    `run_shots` returns 0). goalc-test (`-*MANUAL_TEST*`) 1705 of 1707, failing
    `WorldLightsGlow.ApproachIsSmoothAndNearestEightPops` and `BakedLightsAreNeverEvicted`.
  - Head (after merging origin/master `666b0f333`; pytest with `SANDOVER_ROOT` on the worktree,
    since the bridge-split tests read `game/kernel/common/sandover/bridge.cpp`): pytest 892
    passed, 13 skipped, 1 failed (the export test:
    two export-excluded tool file names in master's `architecture.md` tools row and in the bridge-split
    findings, not in lines this branch adds). goalc-test 1713 of 1715, the same two
    `WorldLightsGlow` failures.
  - All pre-existing on master; this change touches neither the tools nor `WorldLights`.
- Probe (`sandover-capture one`, village1-hut noon, play look): mean 137.7, std 52.2 on every base
  and head instance.
- Pixels: base twice (A1, A2) for the floor, head once (B). The rig is not bit-exact across
  instances here. The floor is non-zero on every shot, so the "max 0 where the floor is 0" clause
  never applies. Set medians (share of pixels more than 8 apart):

  | Set | shots | median share of px > 8 apart, A1-A2 (floor) | A1-B (head) | B-A2 |
  |---|---|---|---|---|
  | fsr3-stills, FSR 3.1 | 8 | 2.52 % | 3.03 % | 1.97 % |
  | fsr3-stills, FSR 4 | 8 | 3.12 % | 3.10 % | 1.76 % |
  | tonemap-hdr (FP16 target, EXR) | 4 | 1.10 % | 1.21 % | 0.47 % |
  | vr-synthetic mono, wave clock pinned | 4 | 2.04 % | 2.29 % | 3.20 % |
  | vr-synthetic XR (emulated PSVR2, 14 shots x main/left/right), wave clock pinned | 42 | 0.42 % | 0.53 % | 0.41 % |

  HDR: EXR max abs per shot, floor against head: 1.86/2.5, 0.182/0.161, 0.849/0.849, 3.81/3.35.
  These are scene-linear maxima at single pixels.
  Head differences are of the same size as the instance-to-instance rig variance, not inside the
  floor on every shot. Shots where base-head is more than 0.1 point above base-base (share of
  pixels more than 8 apart; floor / head / head against the second base run):
  - FSR 3.1 `fountain-particles` 54.72 / 63.19 / 68.89 % (particles)
  - FSR 3.1 `village1-lamps-night` 2.34 / 3.78 / 2.02 %
  - FSR 4 `fountain-particles` 51.28 / 58.19 / 55.55 % (particles)
  - FSR 4 `village1-lamps-night` 3.26 / 3.99 / 1.15 %
  - FSR 4 `village2-crowd-night` 4.28 / 4.53 / 4.49 %
  - HDR `sunken-precursor-glow` 0.96 / 1.26 / 0.68 %
  - mono `jungle-start-mono` (P set) 2.45 / 12.01 / 12.18 %: edges everywhere, a temporal-AA
    phase one instance lands on; A1-A2 showed the same 10.9 % between two base runs
  - mono `village1-hut-mono` 0.13 / 0.49 / 0.29 %
  - XR `sunken-start-xr-nojak` 0.31 / 0.60 / 0.64 % (left 0.34 / 0.62, right 0.41 / 0.80)
  - XR `village1-hut-xr-pitch10` 0.46 / 0.64 / 0.70 % (left 0.48 / 0.70, right 0.53 / 0.69)
  - XR `village1-hut-xr-roll10` 0.42 / 0.69 / 0.63 % (left 0.50 / 0.86, right 0.47 / 0.69)
  - XR `village1-hut-xr-yaw10` 0.32 / 0.58 / 0.60 % (left 0.22 / 0.54, right 0.42 / 0.71)

  The XR excess is 0.2-0.4 points on geometry edges. Where the head is further from one base run,
  it is usually as far from the other, and some base-base pairs exceed base-head (the A1 MSAA
  instance below). These are instance states, not a build difference.
- Sequence gate (`pc-set-xr-fake-option 8 10`, the held-margin capture, emulated PSVR2, village2, game
  paused, wave clock pinned). Frozen: the 10 consecutive frames are bit-identical within each run
  (share > 8 = 0.00000, median abs diff 0) on base, head and base again. Moving (option 7, fixed
  yaw step per frame): the per-pair mean abs diff stays within 1.8 % (left) and 0.4 % (right) of
  its median on all three (pass < 10 %). Base against head per frame: share > 8 at most 1.87 %,
  below the base-base floor (at most 2.11 %) on all 40 eye frames. The real session was not run:
  vrserver was not up.

## What the floor is made of (rig notes)
- Ocean wave phase follows the frame counter (`howto/rig-capture.md`, Water A/B). Unpinned, it put
  B's XR shots 4x further from A1 than A2 was (`village1-hut-xr-yaw10` 1.76 % against 0.43 %),
  all of it on the water. `--goal "(pc-set-water 90 100000000)"` brings head and floor together.
  Pin it for any XR parity run.
- Some instances land on a different temporal-AA phase. The difference then outlines every edge
  (`jungle-start-mono`: 12 % between instances, both base and head, in either pairing).
- The MSAA form (`pc-set-aa 0 0`) with the effects on is not a parity form. Instances disagree on
  53-88 % of pixels (A1 against both others, B and A2 agree at 1.9 %), and one base instance drew
  large black regions over the rivers and coast at `sky-sea-high`. That is on master, outside this
  change, and worth its own issue.
- One base sequence run (A2) came up at another camera. The stage's camera form landed after the
  pose, and seqcmp still passed on it. The pinned re-runs (P1, PB, P2) are the ones counted.

Data: `$SANDOVER_DATA/shots/cleanup-xr/{A1,A2,B}` (stills, hdr, vr, seq),
`{P1,PB,P2}` (vr and seq, wave clock pinned), `stats*.json`, `seq-P.txt`.
