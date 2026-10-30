---
title: Temporal AA per eye in VR (motion vectors, FSR 3.1, FSR 4, GTAO history)
date: 2026-10-28
issue: ""
status: verified on the emulated session (SANDOVER_XR_EMULATE, PSVR2 views) and in debug stereo; the real-session run is pending (headset not connected)
sha: feat/vr-per-eye-upscaling
---
## Answer (5 lines)
Stereo and XR now run the temporal stack once per eye: each eye has its own motion vectors (from
that eye's camera at the previous display frame, so head motion is in them), its own FSR 3.1 or
FSR 4 context and history, and its own GTAO history; both eyes take the same jitter phase and cut
together. The upscaler row is live in the VR profile (DLSS is not offered there). A 5 degree
head step gives each eye's predicted vector field to 0.04 px at the centre, and the per-eye
upscale cuts the display-frame GPU time from 38.9 ms (native MSAA) to 22.6 ms at 67 % and
16.7 ms at 50 % (FSR 3.1); FSR 4 per eye pays its interop twice (32.1 / 27.8 ms).

## What the engine did before
`OpenGLRenderer::render` set `stereo_active` for debug stereo and XR, and that one flag switched
off temporal AA, FSR 3.1, DLSS, FSR 4, motion vectors, motion blur and the GTAO history
(`findings/2026-10-27-vr-bringup.md`, pass table). The headset got MSAA with the effects on, and
GTAO ran a fixed noise pattern with three denoise passes.

Stereo replays the frame's DMA chain once per eye (`dispatch_buckets_stereo`): every renderer runs
twice per display frame, with the eye applied to the cameras (`EyeView`, `apply_stereo_eye`). In
XR the display runs at the headset's rate and the game at 60 Hz, so most display frames redraw a
game frame already shown (`xr_repeat`): `frame_idx` does not advance on those, and nothing
per-game-frame runs again.

## Design
### One history per eye
Every temporal object exists twice and is picked by `SharedRenderState::eye_slot` (0 in mono, the
eye in stereo): `motion_vectors::Renderer`, `Fsr3Upscaler`, `Fsr4Upscaler`, the FSR frame clock,
and `TemporalAccum` inside `SsaoRenderer`. Nothing is shared between the eyes except compiled
programs and the FSR 4 runtime: the second FSR 3.1 context borrows the first one's ten compute
programs (no second 3-4 s compile), and the second FSR 4 context borrows the loaded
`amd_fidelityfx_loader_dx12.dll` and the picked provider but creates its own ffx context, shared
textures, exposure texture and queue timer. Every published VR temporal upscaler keeps separate
history per eye, or one double-wide feature with a seam to keep the eyes apart (research note);
separate contexts need no seam.

### The frame clock: display ticks, not game frames
Each temporal object used to check "the previous frame was mine" with `frame_idx + 1`. In XR that
clock stands still on redraws, and in stereo both eyes see the same value. A new
`display_tick` (+1 per `OpenGLRenderer::render`, redraws included) replaces it in the motion
vectors (frame gap), the GTAO history and merc's per-bucket previous camera; in mono it advances
exactly with `frame_idx`, so mono behaviour is unchanged.

### Motion vectors per eye
The camera velocity pass reprojects with the camera the frame was drawn with; in an eye that is
the eye's camera (game camera, then the head-relative eye rotation and offset, `apply_xr_eye`).
Each eye's renderer keeps its own previous camera, so the reprojection is
`eye camera(t-1) <- eye camera(t)`: game camera motion and head motion in one matrix, per eye,
and the sky (rotation only) follows the head too. Merc object velocity uses the previous bones
(matched per actor, per game frame, as in mono) with that eye's previous merc projection (the merc
setup cameras are now keyed by bucket and eye). On a redraw of a game frame
(`game_frame_new` false) merc takes its current bones as the previous ones: the actors have not
moved since the last display frame, only the head has. A game cut (camera teleport event) cuts only
on the first display frame of its chain, not on every redraw of it. The second eye cuts whenever
the first did (`force_cut`), so both histories restart together. Stereo turning on or off is a cut
of its own (`stereo`, next to `resize`): a toggle at the same render size would otherwise
reproject the mono frame into an eye, or an eye into mono, across the eye offset. The cut makes
the frame invalid, which resets FSR 3.1, FSR 4 and the GTAO history with it.

### FSR 3.1 and FSR 4 per eye
Each eye renders at the eye frame times the render scale and its upscaler reconstructs the eye
frame (`xr_targets->render_w/h`, `set_xr_eye_size`): with today's one symmetric frustum per eye
that is 4491x3468 on a PSVR2 at SteamVR's default resolution, of which 3400x3468 is cropped into
the swapchain, so the swapchain gets the runtime's recommended pixel density times the render
scale before the upscale. The jitter phase is the display tick in stereo (`begin_frame(..., index)`),
the same in both eyes; the jitter goes into each eye's projection only (clip-space offset of the
scene programs), never into the layer submitted to the compositor. The upscaled eye, with its HUD
text buckets drawn after the upscale, is copied into its half of the side-by-side buffer, and the
eye tonemap and crop into the swapchains are unchanged. In the pipeline the XR eye frame is sized
once (`set_xr_eye_size`); the temporal block no longer scales an XR frame a second time.

### GTAO history per eye
`SsaoRenderer` keeps two `TemporalAccum`; the noise index in stereo is the display tick, the same
in both eyes (two eyes with different noise phases would see different AO noise). When stereo turns
off, slot 1's history targets are freed with the second eye's motion vectors and upscalers.

### What stays off in stereo
Motion blur (it would blur by one eye's vectors and is unwanted in a headset), contact shadows
(their row greys in VR now), the background depth prepass, vista hi-Z and the sun disc. No motion
vectors or depth go to the runtime's space warp: SteamVR's motion smoothing does not use the app's
vectors (research note).

### DLSS
Not per eye. The NGX path owns a D3D11 device, the interop device and the NGX init inside one
`DlssUpscaler`; two features on one NGX instance is the right shape but it cannot be run on this
rig (no NVIDIA card), so it is not built blind. In stereo and XR the engine reports DLSS
unavailable (`pc-get-aa-available 1` is 0, the VR menu does not offer it), and a VR file that
names DLSS runs FSR 3.1 per eye.

### Settings
`pc-temporal-aa-state` 2 (stereo or XR) now means "temporal AA runs per eye": `sandover-temporal-aa?`
accepts 1 or 2, so the upscaler and render scale rows are live in the VR profile;
`sandover-stereo-engine?` keeps contact shadows off there (row greyed). The NOT AVAILABLE IN VR
entry and its text id are gone. The stock MSAA row now matters in VR only with vanilla rendering.

## Proof
### Debug stereo, no headset (2026-10-27)
The PSVR2 was not connected (SteamVR: `PlayStation VR2 HMD is not found`), so the first proofs ran
in debug stereo, which takes the same per-eye path without head tracking. village1-hut, noon,
Jak hidden, actors frozen, 1920x1080 per eye, FSR 3.1 at 67 % (1280x720 per eye). Data:
`$SANDOVER_DATA/vr/per-eye/2026-10-27/dstereo3`, `dstereo4` (FSR 4).

| Check | Result |
|---|---|
| Per-eye motion vectors, still camera | valid, `merc_matched` 13 / missed 1 per eye (as mono), velocity 0 (mean 3e-6 px) |
| IPD 0 (both eyes are the mono camera): left against right, FSR 3.1 | PSNR 57.1 / 59.0 dB, mean abs difference 0.07 / 0.04 levels (two shots) |
| IPD 0, each eye against the mono FSR 3.1 frame, static pixels (78 % of the frame) | 41.8 dB both eyes; threshold 38 dB = the deterministic MSAA path's stereo-against-mono gap (38.6 dB: the stereo path skips the bg prepass, vista hi-Z and sun disc) |
| IPD 64 mm, far pixels (> 300 m or sky, 12 %): left against right | 0.36 levels FSR 3.1, 0.36 levels MSAA (no binocular mismatch beyond the MSAA path's) |
| IPD 0, FSR 4: left against right | 47.8 dB, 0.38 levels (below the frame-to-frame noise of 2.2 levels); each eye against mono FSR 4 on static pixels 40.9 dB |
| FSR 4 second context | `fsr4: context 1280x720 -> 1920x1080 ... provider 4.1.2` for eye 1 on its first stereo frame; both eyes dispatch |

### Emulated XR session (2026-10-28, labelled: no headset)
The PSVR2 stayed disconnected, so these ran on `SANDOVER_XR_EMULATE` with the stored PSVR2 views
(`howto/vr-synthetic.md`: no OpenXR instance, the swapchains are GL textures, everything from the
eye targets on is the real path, 90 Hz pacing, the game on its 60 Hz XR tick). Fake head pose,
village1-hut, noon, FSR 3.1 at 67 % (2994x2312 per eye, reconstructed to 4491x3468). Data:
`$SANDOVER_DATA/vr/per-eye/2026-10-28/` (`emu1` image checks, `emu2-control`, `emu3` head step,
`emu4b` cost, `emu5` redraws).

**1. Head step, motion vectors against the geometry** (`sandover-vr-synth mvstep`, Jak hidden,
actors frozen, yaw 0 -> 5 degrees between two display frames). The prediction takes each pixel's
own depth and the two logged eye poses, not the engine's matrices.

| Eye | predicted centre (px) | dumped centre (px) | centre box 64x64 error, max | whole frame error, median / p95 |
|---|---|---|---|---|
| left | (-71.066, 0.002) | (-71.062, 0.002) | 0.044 px | 0.061 / 0.29 px |
| right | (-71.060, 0.002) | (-71.062, 0.002) | 0.044 px | 0.061 / 0.29 px |

Pass (< 0.5 px). tan 5 degrees over the frame's 1.843 half-tangent at 2994 px is 71.1 px. The p95
is set by depth edges, where one pixel's depth belongs to the other surface. Both eyes are valid on
the step frame and zero on the frames before and after (the head was still).

**2. Object motion per eye** (Jak shown, actors live, head still; `SANDOVER_XR_REDRAWS=2`, a dev
knob that redraws each game frame twice more, so redraws can be dumped).

| Frames | object pixels' velocity relative to the camera |
|---|---|
| redraws (`game_frame_new` false), 12 eye frames | 0.0000 mean, 0.0018 px max: no false object motion |
| new game frames, left / right | 0.0447 / 0.0446, 0.0318 / 0.0318, 0.0762 / 0.0763 px mean |
| mono, the same scene (2560x1440 at 67 %), 4 frames | 0.036-0.040 milli-tangent per game step; eyes 0.039-0.094 |

Merc bones match in both eyes as in mono (12-16 matched per eye frame; a few misses on the frame
new actors come into view, as mono has). The eyes agree to 0.3 %.

**3. Ghosting under a head sweep** (the AO-trail fix's wake measure, per eye: mean luma difference on edge
pixels between the frame right after a 12 degree sweep in 0.5 degree steps and the settled frame at
the same pose, minus the MSAA arm's, which has no history; a trail fails above 4 levels).

| Arm | left | right |
|---|---|---|
| MSAA (floor) | 2.19 | 1.40 |
| FSR 3.1 per eye, 67 % | +2.66 | +2.65 |
| FSR 4 per eye, 67 % | +2.10 | +2.08 |
| FSR 3.1 with the camera part of the vectors zeroed (`SANDOVER_MV_STATIC=1`, negative control) | +7.27 | +7.32 |

Pass; the control fails, so the measure sees a history that does not follow the head.

**4. Binocular mismatch at identity**: on far pixels (> 300 m or sky, 11 % of the frame) the
side-by-side halves differ by 0.27 levels, against 7.4 levels between two shots of the same eye
2 s apart (water and clouds move).

**5. FSR 4 second context**: created for eye 1 on the first stereo frame (`fsr4: context
2994x2312 -> 4491x3468 ... provider 4.1.2`), its own shared textures and queue timer.

## Cost
GPU frame per display frame (`PERF-GPU name=frame`, both eyes, three 60-frame windows after one
warm-up window; emulated session, one gk, other work on the GPU not excluded):

| Arm | render size per eye | frame | upscaler (both eyes) | motion vectors | GTAO |
|---|---|---|---|---|---|
| MSAA 2x, 100 % (before this change) | 4491x3468 | 38.9 / 38.6 ms (two sessions) | | | 3.22 |
| FSR 3.1, 100 % (native TAA) | 4491x3468 | 38.5 ms | 6.05 | 0.30 | 2.31 |
| FSR 3.1, 67 % | 2994x2312 | 22.6 ms | 4.13 | 0.14 | 0.95 |
| FSR 3.1, 59 % | 2642x2040 | 19.5 ms | 3.79 | 0.11 | 0.72 |
| FSR 3.1, 50 % | 2246x1734 | 16.7 ms | 3.38 | 0.08 | 0.51 |
| FSR 4, 67 % | 2994x2312 | 32.1 ms | 14.21 | 0.12 | 0.94 |
| FSR 4, 59 % | 2642x2040 | 29.5 ms | 13.84 | 0.09 | 0.71 |
| FSR 4, 50 % | 2246x1734 | 27.8 ms | 13.61 | 0.07 | 0.52 |

FSR 4's span is the interop: per eye the D3D12 dispatch is 4.0-4.3 ms on the queue, and the
render thread waits 1-9 ms for the hand-over and 5-6 ms for the hand-back, twice per display frame
(`fsr4: cpu ms` lines, `eye 0` / `eye 1`). The bridge's per-hand-over cost
(`2026-10-22-fsr4.md`, open issue) is paid per eye. None of these reach 11.1 ms (90 Hz); at 67 %
FSR 3.1 the frame is 1.7x faster than before. The real-session numbers (SteamVR compositor
included) are pending with the headset.

### VRAM for the second eye
What the second eye allocates on top of mono, at the VR default (PSVR2 at SteamVR's default
resolution, 67 %): render 2994x2312, reconstructed eye 4491x3468, HDR scene (RGBA16F), no MSAA
under the upscaler, GTAO at its default half resolution. Sizes from the allocation code, MiB.

| Path | what | MiB |
|---|---|---|
| FSR 3.1 | 13 render-size targets (44 B/px), half-size SPD chain, shading change, farthest depth (half size), locks, two upscaled RGBA16F and the display colour + depth24s8 (29 B/px at 4491x3468) | 735 |
| FSR 4 | shared textures: colour, depth, velocity (16 B/px at render size) and output RGBA16F (4491x3468), plus the display colour + depth24s8; the ffx context's own D3D12 resources are not counted | 403 |
| GTAO | two RG16F history targets at 1497x1156 (53 MiB if GTAO runs at full resolution) | 13 |
| Motion vectors | velocity RG16F, aux RGBA16F, colour and history copies RGBA16F, object velocity RGBA16F, all at render size | 238 |
| **Total, FSR 3.1 per eye** | | **986** |
| **Total, FSR 4 per eye** | | **654** + the ffx context |

Only one upscaler runs, so a second eye costs about 1 GiB of VRAM with FSR 3.1 and about
0.65 GiB plus the FSR 4 runtime's own history with FSR 4.

FSR 4 interop per eye, one steady-state frame of the 67 % run (`emu4b`, eye 1): hand-over
6.86 ms, record + submit 0.07 ms, hand-back 5.39 ms of render-thread time around a 4.04 ms D3D12
dispatch, so about 12.3 ms of CPU wait per eye, paid twice per display frame.

## Pending
- The same runs on the real session (`SteamVR`, PSVR2) once the headset is connected: the
  steps files are the emulated ones without `SANDOVER_XR_EMULATE`, frame time from the `[xr]
  timing` lines.

## Follow-ups
- DLSS per eye (two NGX features on one init), on a machine with an NVIDIA card.
- Asymmetric per-eye frusta (3400 instead of 4491 columns): the upscalers then reconstruct
  exactly the swapchain size and a quarter fewer pixels.
- Repeat-frame resubmission instead of a full redraw would halve the per-eye upscaler work at
  90 Hz from 60 Hz content (vr-bringup note).
