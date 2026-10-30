---
title: VR display frames held at 60 fps after a vsync change, and where a display frame's time goes
date: 2026-10-30
issue: ""
status: verified (real SteamVR session with the PSVR2, fake head, one game instance on the machine)
sha: perf/vr-frame-pipeline
---
## Answer (5 lines)
A vsync change during an XR session (the display-mode switch flips the setting) called
`SDL_GL_SetSwapInterval(1)` on the mirror window. The desktop runs at 60 Hz, so from then on every
render-loop iteration, which is one headset frame, waited for a 60 Hz vblank: the session sat at
exactly 60.0 fps. The setting now reaches the window only when the session ends. Above that, a
display frame is GPU-bound; the next lever is one asymmetric frustum per eye (about 24 % fewer pixels).

## Symptom
Headset play at 60.0 fps flat after a display-mode change, with frame times that should allow
more; the `present:` line read `avg 16.67 ms (60.0 fps)` in every window.

## Mechanism
- `GLDisplay::render` sets the window's swap interval to 0 when a session starts (`xrWaitFrame`
  paces the loop) and back to the user's vsync when it ends. A separate branch applies a vsync
  change whenever `vsync != old_vsync`; it called `SDL_GL_SetSwapInterval(vsync)` in a session
  too, so a flip re-armed interval 1 for the rest of the session.
- With interval 1 the mirror's `SwapBuffers` blocks for the desktop's vblank (60 Hz here, the
  headset is not the desktop display). The swap is on the render thread, after the eyes are
  submitted, so each headset frame took at least 16.67 ms.
- Now the branch only records the new value in a session; the session-end path applies it.
  The `present:` line logs the window's swap interval.

## Proof
Village2 play camera, FSR 3.1 (the VR profile), fake head, one game instance on the machine
(sampled every 2 s), 5 s present windows:

| | after a vsync off/on flip, 33 % render scale |
|---|---|
| master, headset idle (session SYNCHRONIZED) | 16.67 ms (60.0 fps) in both windows |
| fix, headset idle | 15.3-15.6 ms (63.9-65.4 fps), swap interval 0 |
| fix, headset awake (session VISIBLE) | 16.57-16.68 ms avg, min 14.4 ms, swap interval 0 |

A frame above 16.7 ms does not reach the cap, so the fix changes nothing there (headset awake,
back to back, after the flip): 33 % master 22.6-23.6 ms, fix 21.1-24.1 ms; 67 % master
30.7-31.0 ms, fix 31.1-31.3 ms.

Sequence gates on the fix (paused, 10 consecutive display frames per eye, no upscaler): still
head 0.071-0.082 % of pixels more than 8 apart (left), 0.019-0.020 % (right), bound 0.1 %;
60 deg/s sweeps within 0.9-2.1 % of their median (bound 10 %). The left eye's extra pixels sit
on the river at the lower left of its outer field (animated water while paused), not on edges.

## Where a display frame's time goes
Same camera, headset awake, per-frame means over two 60-frame windows. 33 %: the fix build;
67 %: the same eye rendering (an earlier build of this branch) from the session the runtime
throttled to 45 fps (the 60 fps session's 67 % window has a 6.6 ms vista spike in eye 0):

| | 33 % | 67 % |
|---|---|---|
| display frame (present) | 16.6 ms | 31.1 ms |
| GPU frame | 16.8 ms | 31.2 ms |
| eye scene, eye 0 / eye 1 | 7.6 / 4.8 ms | 15.7 / 10.9 ms |
| FSR 3.1 per eye | 1.7 ms | 2.7-2.8 ms |
| render thread: `xrEndFrame` (waits for the GPU) | 7.1 ms | 22.0 ms |
| render thread: `xrWaitFrame` | 0.02 ms | 0.02 ms |
| render thread: eye submission, eye 0 / eye 1 | 4.9 / 4.1 ms | 4.8 / 4.0 ms |

- GPU-bound at both scales: `xrWaitFrame` returns at once and `xrEndFrame` waits for the GPU.
  Above 33 % the eye scene grows with its pixels; at 33 % FSR 3.1 at the full output size and the
  per-eye submission (4-5 ms of CPU each, with no overlap with the GPU while `xrEndFrame` drains
  it) set the floor.
- Eye 0's scene costs 2.8 ms more than eye 1's at 33 %. The passes that run in eye 0 only or
  mostly (emissive-light tracking, vista, merc, etie, bucket 9) account for about 1.3 ms of
  it; the rest is not attributed.
- The output is 4491x3468 per eye, not the runtime's 3400x3468: one symmetric frustum covers
  both eyes' asymmetric fields (half-tangent 1.842, the 61.5 degree outer side), so each eye
  draws its outer side's width on the inner side too. One frustum per eye at the runtime's
  recommended size is 24 % fewer output pixels (eye scene, FSR and copy). Not done here.
- Pose age of the game's view at its last display: 33.5 ms at 33 %, 56 ms at 67 %; the largest
  stage is the game's own wait for its 60 Hz vsync before it sends the chain (16.0 ms and
  27.5 ms). Sending the chain before that wait, locating the head again when the game takes its
  view and waking the game before `xrEndFrame` moved the wait between stages (33.5 ms before
  and after the first two, 37-40 ms with the third, headset idle); they are not in this change.
- Run to run, the same build and camera measured 16.8 ms and 21.0 ms GPU frames in two awake
  sessions 12 minutes apart (the runtime throttled one to 45 fps); compare builds only back to
  back in one runtime state.

## XR keep-alive on SteamVR (measured here, not changed)
Master play build on a fresh copy, `SANDOVER_XR_KEEPALIVE=1`, `--xr` from boot, the launcher's
effect forms, village1 then the beach, headset awake and not worn. No OpenXR errors. From the
session start to the first lit frame: one render-thread gap of 5.4 s with no empty frame, then
the keep-alive's empty frames 3.0-5.3 s apart at worst (174 empty frames over a 15.0 s stall,
gaps over 500 ms: 5413, 5338, 699, 797, 3080, 670, 809, 3466, 2964 ms). After the boot: two
0.7-0.8 s gaps around the warp to village1, none at the effects or the beach. On the emulated
session the same thread kept every gap under 165 ms, so on SteamVR its own frame calls wait
during the stalls; where was not isolated. What the headset showed during the gaps was not
observed. The keep-alive stays off by default.

## Follow-ups
- One asymmetric frustum per eye at the runtime's recommended size (24 % fewer output pixels).
- Overlap the next frame's CPU work with the GPU instead of draining it in `xrEndFrame`.
