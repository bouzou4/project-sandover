---
title: First headset run of the OpenXR path since the temporal stack (PSVR2 on SteamVR)
date: 2026-10-27
issue: ""
status: verified
sha: fix/vr-openxr-bringup
---
## Answer (≤5 lines)
The session comes up on the PSVR2 through SteamVR 2.18.4 and head tracking works, but three
things broke play. A level that finished loading between the two eye passes crashed the game
(TIE wind snapshot of the wrong size). After SteamVR ended a session, every retry failed with
`XR_ERROR_CALL_ORDER_INVALID`. With the FP16 target on, the headset got the scene before the
tonemap. All three are fixed. Frame rate is the open problem: 42 ms per display frame against an 11.1 ms budget.

## Evidence
Three runs with the play launcher (`play.py --vr`, all effects, HDR presenter on), the headset
worn part of the time. Logs: `$SANDOVER_DATA/vr/2026-10-27/` (`gk-smoke1.log`, `gk-smoke2.log`
on the master build, `gk-run3.log` on this branch).

Runtime facts, from the `[xr]` lines:
- System `SteamVR/OpenXR : playstation_vr2`; recommended 3400x3468 per eye; display period
  11.1 ms (90 Hz) in two runs and 8.3 ms (120 Hz) in the first.
- Swapchain formats, runtime order: RGBA16, RGBA16F, RGB16F, SRGB8, SRGB8_ALPHA8 and three
  depth formats. No plain RGBA8; the engine picks SRGB8_ALPHA8.
- Per-eye field of view: 61.5 deg outward, 43.45 deg inward, 53.04 deg up and down. The engine draws
  one symmetric frustum that covers both eyes (half-tangents 1.842 x 1.329), so each eye is
  rendered at 4491x3468 and cropped to 3400x3468.
- The session stays `SYNCHRONIZED` (no frames requested, `submitted 0/450`) until the
  headset is worn. It goes `VISIBLE`/`FOCUSED` within a frame of putting it on, then back to
  `SYNCHRONIZED` when it comes off.

### 1. Crash: `Assertion failed: 'info.wind_idx * 4 <= m_wind_vectors.size()'` (Tie3)
Under stereo, `Tie3::render` snapshots the wind springs on the first pass of a chain and
restores them on every repeat pass (eye 1, and every redrawn XR display frame), so that both
eyes integrate wind from the same state. When a level's TIE finishes loading after the snapshot,
`try_loading_level` resizes the wind vector to the new level's size, and the next repeat pass
copies the old, smaller snapshot back over it. The first wind draw then indexes past the end.
In smoke run 2 this killed the game 2 s after the headset went `FOCUSED`, while village1 was
still streaming in. The restore now runs only when the sizes match. Otherwise the pass keeps
the loaded vector and takes a new snapshot. Run 3 went through the same village1 load while
`VISIBLE` and kept running. Mono is untouched: the code is inside `stereo_active`.

### 2. No session after the runtime ends one
Smoke run 1 rendered at 2.3 fps while `VISIBLE` (35 s frames: another process held most of the VRAM
then). SteamVR moved it `VISIBLE -> SYNCHRONIZED -> STOPPING -> IDLE -> EXITING`. The engine
destroyed the session, kept the instance and called `xrCreateSession` again 3 s later, which
SteamVR refused with `XR_ERROR_CALL_ORDER_INVALID`. The engine then sat in `FAILED` for 10 s and
retried the same way. `EXITING` and `LOSS_PENDING` now drop the instance as well and restart from
`xrCreateInstance`. That is the same path as the first start, which succeeds. Not reproduced on
purpose: it needs SteamVR to end the session.

### 3. Headset image before the tonemap
With the FP16 target on, the stereo buffer holds the scene before the tonemap. The window gets the
final pass (tonemap and HDR encode). The eye copies blitted the FP16 values straight into the
SRGB8_ALPHA8 swapchain images, so the headset showed values above 1 clipped hard, not the
toon roll-off. The eyes are now tonemapped once into an RGBA8 copy, with the same shader and
uniforms as the internal-resolution screenshot and without the HDR encode, and then cropped
into the swapchains. Size of the difference on a 4K HDR frame (internal-resolution EXR against its
tonemapped PNG, toon): 0.17 % of pixels are above 1, and 0.1 % differ by more than 4 levels in 8
bits. The difference is in the highlights only (eco, emissive, sun glints), because toon is close to identity below its knee.
With the FP16 target off, the copy is unchanged.

### Passes under stereo (code read; nothing is shared between the eyes)
| Pass | Under stereo/XR |
|---|---|
| Temporal AA, FSR 3.1, DLSS, FSR 4 | off (`OpenGLRenderer::render`) |
| Motion vectors, per-object motion blur | off (`MotionVectors.cpp`, `MotionBlur.cpp`) |
| GTAO history | off: no motion vectors, so a fixed noise pattern and 3 denoise passes (`SSAO: no motion vectors` in the log) |
| Contact shadows | off (`ContactShadows.cpp`, also when an eye view is active) |
| Background depth prepass | off (`OpenGLRenderer.cpp`, `bg_prepass`) |
| Vista hi-Z occlusion | off (`VistaRenderer.cpp`) |
| Sun disc | off (`SunDisc.cpp`); the sky has no sun disc in the headset |
| Stencil marks, SSAO (fixed pattern), tile lights, bloom | per eye, from that eye's own depth and normals; tile lights reuse eye 0's tracked light list (one readback per frame) |
| HDR presenter | window only; the headset now gets the SDR tonemap |
The history-based passes are all off in stereo, so no eye reads another eye's history.
Per-eye history is the norm in VR (background research for the temporal work).

### Frame time
| Run | State | Display interval | New frames/s |
|---|---|---|---|
| 3 | `SYNCHRONIZED` (not worn) | 11.15 ms | none drawn |
| 3 | `VISIBLE`, village1 | 42-43 ms | 23-24 |
Every display frame draws the scene twice at 4491x3468, 31.2 Mpx in total. That is 3.8x a 4K frame,
with every per-pixel pass (SSAO, tile lights, bloom, water) running per eye. The 42 ms is in line
with that pixel count, so the cost scales with fill rate, not with the XR plumbing.

### Also seen, not fixed
- One burst of 32 `glDrawBuffers ... '0x8ce0' ... GL_INVALID_OPERATION` errors in the tfrag,
  tie, merc and generic buckets about a second after a level's materials upload, in both XR
  and the first mono frames after XR. `COLOR_ATTACHMENT0` is invalid only on the default
  framebuffer, so one of the draw-buffer helpers ran with framebuffer 0 bound.
  `SANDOVER_GL_STATE_CHECK=1` logged no stale copy. Not traced further.
- A blocking screenshot (`pc-screen-shot`) during a session re-creates the frame targets at
  the screenshot size and stalls the loop for 10-15 s. Do not take screenshots while the headset is worn.

## Implications for the design
- 90 Hz needs about 4x fewer pixels per display frame. In order of size: the SteamVR
  per-application resolution (the runtime's own knob; the recommended size follows it),
  each eye drawn with its own asymmetric frustum instead of the shared symmetric one
  (3400 instead of 4491 columns, -24 %), and resubmitting the last eye images for display frames that
  carry no new game frame, letting the compositor reproject them (-33 % at 90 Hz from 60 Hz
  content, at the cost of head translation on those frames).
- Temporal AA in VR needs per-eye history and per-eye motion vectors; the runtime's own
  motion smoothing takes none of the app's vectors.

## Follow-ups
- Per-eye asymmetric projection in `EyeView` (all eye-aware renderers and `xr_gs.glsl`).
- Repeat-frame resubmission instead of a full redraw (measure against the redraw).
- The default-framebuffer `glDrawBuffers` burst.
