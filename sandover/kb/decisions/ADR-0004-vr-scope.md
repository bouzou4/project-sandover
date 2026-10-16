# ADR-0004: VR scope for the PSVR2 stereo phase
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The original specification sketched a gaze-directed rendering phase (foveation informed by eye
tracking) on top of stereo PSVR2 support. The tooling evaluation and the OpenXR environment check evaluated the concrete stack: OpenXR
(Khronos SDK, Apache-2.0) + SteamVR (native `XR_KHR_opengl_enable` since 2020) is a solid
stereo path. Gaze, however, only reaches this engine through PSVR2Toolkit's own custom
IPC/C API today — confirmed against its upstream source tree, not through the standard
`XR_EXT_eye_gaze_interaction` extension end to end on this stack — and PSVR2Toolkit's
README asks for non-commercial use only, layered on top of its MIT license file.

## Options (with cost/risk)
**Full scope: stereo + gaze via PSVR2Toolkit (the original plan).** Cost: couples the
engine to an experimental, pre-1.0 third-party driver overlay with an unresolved licensing
ask and a still-changing native API. Risk: highest — non-commercial licensing conflict,
IPC layout breakage across toolkit builds, runtime contention from other installed OpenXR
runtimes (Meta/Oculus, Virtual Desktop) muddying gaze capture.
**Stereo only via OpenXR + SteamVR, no gaze (chosen).** Cost: no foveated rendering or
gaze-directed effects this phase. Risk: low — OpenXR/SteamVR path is mature and the
licensing/stability risk of PSVR2Toolkit is fully avoided.

## Decision
Stereo 3D only, via the OpenXR Khronos loader + SteamVR runtime with
`XR_KHR_opengl_enable`. Gamepad input is unchanged. The HUD renders on an OpenXR quad
layer. Presets are diorama-first. D3D11 interop (`WGL_NV_DX_interop2` /
`XR_KHR_D3D11_enable`) is kept as a benchmarked contingency, not a required build, against
SteamVR's known OpenXR compositor swapchain-copy overhead.
**No eye tracking, no gaze, no PSVR2Toolkit, no motion controls.** This supersedes the
the original gaze plans.

## Consequences
- `kb/findings/2026-09-23-vr-stack.md` and `2026-09-23-vr-environment.md` keep their
  gaze/PSVR2Toolkit findings as historical record only; treat as refuted/moot, not a plan.
- No PSVR2Toolkit binaries are vendored, redistributed, or depended on by the engine.
- If gaze is revisited later, re-run the OpenXR environment check first (other OpenXR runtimes
  can silently steal `ActiveRuntime`) and get a licensing answer on PSVR2Toolkit first.

## Amendment (2026-09-23)
The target is any OpenXR headset through SteamVR (or another OpenXR runtime), not one specific device. No device-specific optimizations, resolutions or panel assumptions in the design; per-eye budgets derive from the runtime's recommended swapchain size at run time. The headset on hand is only the test device.
