---
title: Synthetic eye images through the real OpenXR path; why the headset lost the sky, Jak and foliage
date: 2026-10-27
issue: ""
status: verified (framebuffer leaks, eye geometry); open (sky band, far scenery)
sha: fix/vr-synthetic-rig
---
## Answer (5 lines)
The eye geometry is right: with the runtime's real per-eye field of view and IPD, every tested head
pose (identity, yaw, pitch, roll 10 degrees) puts the world exactly where the prediction says, in both
eyes, and the eyes are neither swapped nor mis-cropped. The headset lost the sky, the world and Jak
because passes before the bucket loop left another framebuffer bound, so the buckets drew off target.
Two leaks plus the prepass restore are fixed, and the buckets now always start on the frame's target. Still wrong: a black
band between the sky and the horizon that does not follow the head.

## How the images are made
`howto/vr-synthetic.md`. The OpenXR session is real (SteamVR 2.18.4, PSVR2, `VISIBLE`), only the
head pose is scripted. The eye images are read back from the swapchain images before release.
Runtime facts: both views have identity orientation in VIEW space (no canting), x = -/+30.5 mm
(IPD 61.0 mm), fov 61.50 outward / 43.45 inward / 53.04 up and down, 3400x3468 per eye. Stored in
`$SANDOVER_DATA/vr/psvr2-profile.json`. Data: `$SANDOVER_DATA/vr/synthetic/2026-10-27/`.

## Measurements (current build, before the fixes; the geometry does not change with them)
| Test | Result |
|---|---|
| (a) disparity sign and growth | pass: xL - xR on the shared-frustum halves is 0 in the sky rows and +8 to +16 px in the bottom third at 4491 px width, all four cameras (positive: near objects sit right in the left eye, as parallel eyes give) |
| (b) horizon tilt at identity, eyes agree | pass: residual 0.0 degrees in both eyes, all cameras |
| (c) eye distance vs runtime IPD and `stereo_world_scale` | pass (log): `head_x_m` -/+0.0305 = half the runtime IPD; at world scale 1 the eyes sit -/+125 game units from the head. Not checked against a measured depth in the image |
| (d) asymmetric projection, no eye swap | pass: the mono shot warped through each eye's own tangents (left -1.842..0.947, right -0.947..1.842) registers with zero shift and scale 1.00 |
| (e) head rotation | pass: yaw, pitch and roll 10 degrees register with zero residual in both eyes; the identity prediction against the pitch image is off by 232 px (tan 10 degrees x 1305 px per tangent = 230), so the test sees a wrong pose. The real tracked pose (headset on the desk, pitch 4.8, roll 1.6 degrees after the yaw-only recentre) also registers with zero residual |
| Sky | fail: black band from tangent +0.72 down to the horizon in every outdoor eye image; its upper edge stays put under pitch and roll |
| Far scenery | fail: the distant islands and rock spires above the village horizon in the mono shot are missing in the eyes |

The suspects raised earlier: the per-axis eye scale (0.452 x 0.353) is correct (the game's 16:9 frustum
is not stretched; the registration finds scale 1.00); the crop from the symmetric frame is correct;
the eye offset sign and `stereo_swap_eyes` are correct; the recentre's yaw from view 0 equals the
head's yaw on this headset (the views are not canted).

## The framebuffer leaks (sky, world and Jak gone)
Draw inventory per eye (`SANDOVER_BG_PROBE`, `sandover-vr-synth inventory`): sky, tfrag, tie, shrub,
merc, sprite and ocean submit draws in both eyes on every XR frame. What breaks is the target. The
buckets never bind the render target themselves: whatever the passes before them left bound is
where sky, tfrag, tie and shrub go (merc and the MRT draw-buffer helpers then fail on it, which is
the `glDrawBuffers ... 0x8ce0` burst in the headset log at the moment the session went `VISIBLE`).
The probe now reads the binding after each stage before the buckets:
- `ActorShadows::prepass` left FBO 0 bound on the first XR frame (`stages actor-shadows:0`). Inside
  it, `WorldShadows::ensure_target` allocates the 192 MB cascade array on first use and ends with
  `glBindFramebuffer(0)`; the pass's `SavedState` (cached, perf-opt geometry bit) then captured
  that 0 as the state to restore and keeps restoring it for up to 256 frames, until the cache
  refreshes or is invalidated: 3 s at 90 Hz, 11 s at the 23 fps the headset ran at. In the headset run the
  allocation happened on the first eye frame (`world-shadows: 4096x4096 x 3` at the same second as
  the errors).
- `motion_vectors::Renderer::ensure_targets` left its history FBO bound on every resize (rig mono
  shots: `stages motion-vectors:75`; the rig's black-world frames of `2026-10-23-rig-black-world.md`).
Fixes: `ensure_target` and `ensure_targets` put back the binding they found; `ActorShadows::prepass`
rebinds the frame's render target on every exit (no `glGet`: the target is known); `dispatch_buckets_jak1` binds the render target
right before bucket 0. Proof with the rebind disabled (`SANDOVER_BG_PROBE_NOGUARD=1`), XR with the
HDR presenter as the play launcher sets it: old build `pre:0 stages actor-shadows:0`, buckets 0-18 on
FBO 0; new build 0 off-target lines in 10 eye frames and 4 mono shots, no `0x8ce0` errors.

The eye PNGs cost two synchronous full-size readbacks and PNG encodes on the render thread before
`xrEndFrame`, so they are written only when the head pose is scripted (`FrameTargets::fake_head`):
an F2 screenshot in a worn session writes the side-by-side PNG alone. The `ensure_target` query
runs only on reallocation, so the frame path gains no `glGet` (the deep-optimization-pass contract).

## Open
- The sky band: skipping bucket 3 (`SANDOVER_BG_PROBE_SKIP=3`) removes the blue, so the sky draws
  only above tangent +0.72 and nothing below it. The sky's lower edge does not move under pitch or
  roll while the terrain does, so the sky's GS-space triangles are not following the eye (the
  `xr_gs` at-infinity path in `SkyRenderer`'s direct renderer). Turning off the GS scissor for
  reprojected draws changed nothing. Next: log the sky vertices' GS coordinates and the `xr_gs`
  uniforms for one eye frame.
- The far scenery missing above the horizon (vista neighbours or far tfrag): not traced.
- Not reproduced here: a loss over many frames. In the rig the leak hit single frames (each shot
  invalidates the cache); in the headset run nothing invalidated it, so the cached 0 held for up to
  256 frames per allocation, but that run predates the probe.

## Follow-ups
- Sky and far scenery in the eyes (above).
- The first XR shot of a rig boot misses `RIG:RELEASED` (slow first eye frames).
