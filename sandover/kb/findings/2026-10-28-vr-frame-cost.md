---
title: VR frame cost: the FSR 4 interop lock taken once per display frame, no HDR present in XR
date: 2026-10-28
issue: VR play check 2026-10-28 02:30: 31 ms per display frame with FSR 4 at 33-50 %
status: verified on the emulated PSVR2 session (real-session numbers pending a free headset)
sha: perf/vr-frame-cost on master 627939c5c
---
## Answer (5 lines)
At the play VR settings (FSR 4, render scale 33 %, SSAO and shadows low) the render thread spent
two thirds of each display frame blocked in two D3D12 interop hand-backs and one HDR present
lock, with the GPU idle behind them. Eye 0's FSR 4 dispatch now runs on the D3D12 queue while GL
draws eye 1, both eyes come back in one lock, and an XR session presents the window mirror
through the GL swap. Village2, emulated session: 29.1 -> 19.3 ms at 33 %, 31.5 -> 23.8 ms at 50 %.

## What the frame did
Per display frame, both eyes in turn: the scene, the FSR 4 input copy, `wglDXUnlockObjectsNV`
(the driver blocks until GL has finished everything queued, 2-4 ms), the D3D12 dispatch (4 ms
per eye at a 4491x3468 output), `wglDXLockObjectsNV` (blocks until the queue is idle, 5-6 ms),
then the output blit. While the render thread waits in the lock, GL has nothing queued; while the
queue runs FSR 4, GL is idle. After both eyes the window mirror went through the HDR presenter,
whose interop unlock drains GL once more (2.7 ms of GPU timeline, 2.8 ms of render thread). The
bridge cannot wait on the GPU instead (`2026-10-23-bridge-semaphores.md`: the AMD GL driver's
semaphores do not hold), so the stalls can only be overlapped or removed.

## The change
1. **FSR 4 hand-back once per frame.** In XR, when nothing draws into an eye after its upscale
   (the VR 2D layer takes the text buckets; the SSAO and motion-vector debug views are off),
   eye 0 submits its dispatch and returns without the hand-back or the output blit. GL goes on
   with eye 1; eye 1's hand-back locks both eyes' targets in one call; eye 0's output blit and
   its copy into the side-by-side frame follow. Eye 0's FSR 4 is the same dispatch on the same
   inputs; only the CPU order of its hand-back changed. Mono, debug stereo, FSR 3.1 and XR
   without the 2D layer take the old path. `SANDOVER_XR_FSR4_SERIAL=1` (dev) turns it off.
2. **No HDR present in an XR session.** The eyes are sRGB swapchain images tonemapped to SDR
   (`tonemap_to_sdr`); the window is a mirror. While a session runs the HDR presenter is not
   started (`hdr::update(..., allowed = false)`), so the mirror goes through the GL swap with
   no interop lock. The HDR output row and HDR calibration grey out in the VR profile, since they
   change nothing there; the flat profile's HDR comes back when the session ends.

## Proof (emulated PSVR2 session, village2 F7 camera of run 20261028-0230, hour 13, actors frozen)
Present intervals (`present:` lines, 5 s windows after settling; windows with another gk instance
running are left out). Master = 627939c5c.

| Arm | master | (2) only (`SANDOVER_XR_FSR4_SERIAL=1`) | (1) + (2) |
|---|---|---|---|
| FSR 4, 33 % | 29.1 ms | 21.3 ms | 19.3 ms |
| FSR 4, 50 % | 31.5 ms | 25.0 ms | 23.8 ms |
| FSR 3.1, 33 % | 16.7 ms | 11.1 ms (90 Hz pacing) | 11.2 ms |
| FSR 3.1, 50 % | 17.2 ms | 15.6 ms | 14.7 ms |

Render thread at FSR 4 33 % (`PERF-CNT`): eye 0's interop time 6.4 -> 1.7 ms, eye 1's 6.7 ->
8.8 ms (it now also waits for eye 0's dispatch, which ran under eye 1's scene), frame 21.8 ->
19.3 ms. Eye images: on the 2D-layer base, eye 0 deferred against serial differs by 1.06-1.40
levels (mean absolute, 8-bit), the same as two shots 3 s apart (0.98-1.10) and as eye 1 (0.81-
1.10), so the left eye is the same image.

## Pending
- The same arms on the real SteamVR session: `xrEndFrame` there waits for the GPU (13.5 ms of
  render thread at FSR 3.1 50 %), which may hide part of (2).
