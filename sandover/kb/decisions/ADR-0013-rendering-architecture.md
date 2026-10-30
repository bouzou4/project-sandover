# ADR-0013: Rendering architecture for point lights and temporal effects
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The engine's bucket renderers are forward, PS2-blend-faithful, and MSAA is the only AA.
Point lights need a path that does not require rewriting those renderers, and SSAO
(ADR-0009) needs a depth source and a place to composite without breaking actor AO or
transparency blending.

## Options (with cost/risk)
**Full deferred G-buffer (rejected).** Forces a rewrite of every bucket renderer to fit a
G-buffer layout, breaks MSAA's per-sample resolve semantics that the existing blend passes
rely on, and adds real cost to transparent buckets (31-46, 57-66) that a deferred pass
does not shade. Highest parity risk against the original PS2 look.
**Forward+ with compute light culling (rejected).** Needs SSBOs/compute shaders; the
engine targets OpenGL 4.1 for platform reach, which has no compute stage. Not available.
**Screen-space light pre-pass at the opaque boundary (chosen).** Opaque passes add a
normal target (merc uses real vertex normals; tfrag/tie use fitted normals from the
unbake data, falling back to derivative-based face normals where unbake has no fit).
CPU-tiled light lists shade a lighting buffer from that normal target and the bucket-30
depth capture (`kb/findings/2026-09-23-ssao-plumbing.md`). Every forward shader adds
the buffer's value at its own pixel; transparent draws sample the same buffer at their
pixel rather than re-shading. No G-buffer, no compute, no renderer rewrite.

## Decision
Light pre-pass, built on the existing bucket-30 depth capture. No deferred renderer. MSAA
stays the only AA; there is no TAA. Temporal accumulation is scoped to SSAO only, with
camera-only reprojection and a disocclusion fallback (no object motion vectors yet). SSAO
composites after the pris buckets (48-56), gated by a stencil mask that opaque draws set
and transparent draws clear, so characters get contact AO and transparents do not.

## Consequences
- Temporal prerequisites for later work: retain the previous frame's view-projection
  matrix; merc would additionally need previous-frame skinned positions for object motion
  vectors, not yet built.
- DLSS and path tracing are out of reach on the OpenGL backend (their SDKs target D3D or
  Vulkan) and are not planned.
- Source: `kb/findings/2026-09-23-ssao-plumbing.md`.

## Amendment (2026-09-23)
Point-light composite, corrected: background buckets have already drawn when the mid-frame depth exists, so their lighting cannot be added in their own shaders. Background passes write albedo to a second target; after the actor buckets, a screen-space pass computes point-light contributions and composites them as light × albedo over opaque pixels (stencil-masked). Characters draw after that depth exists and take per-draw light lists in the merc shader instead. A second multisampled depth copy after the actor buckets (bucket 56) provides character contact AO; the bucket-30 copy remains for background AO and any pass that must exclude characters. The stencil convention is inverted so bit 7 means "exclude from AO" and the shadow renderer keeps bits 0–5. FP16 is not parity-neutral even with a clamp tonemap; it is a preset choice, never on in the faithful anchor, and the gap is measured before shipping.

## Amendment (2026-10-18): FSR 3.1 temporal antialiasing and upscaling
Supersedes "MSAA stays the only AA; there is no TAA" and, for these passes only, the
`#version 410 core` rule (ADR-0009). AMD FidelityFX FSR 3.1 (the temporal upscaler, not frame
generation) runs on the velocity buffer from `feat/motion-vectors`: at render scale 1 it
is the temporal antialiasing, below 1 the upscaler. It renders the scene single-sample.
- Temporal AA is the enhancement path; MSAA is vanilla rendering (upstream's path) and stereo or
  XR only. SMAA and FSR 1 are removed.
- Compute exception: the FSR passes are GL 4.3 compute programs (`#version 430 core`), built from
  the SDK's GPU headers vendored unmodified under `third-party/fidelityfx` plus an OpenGL callbacks
  header and pass entry points of our own (`shaders/fsr3/`). Every other shader stays
  `#version 410 core`. The context asks for 4.3 everywhere but macOS (4.1); a capability check at
  boot (GL 4.3, compute entry points, headers present) and a failed program build both report the
  mode unavailable, and the game then runs vanilla rendering.
- Placement: scene, SSAO, tile lights, bloom and sprites at render size; the velocity pass; FSR at
  the end of the sprite bucket; the debug, HUD text and
  subtitle buckets at display size after it; tonemap and HDR encode in the final pass, unchanged.
- Jitter: a Halton (2, 3) sub-pixel offset over the SDK's phase count, added in clip space after
  the vertex stage of every scene program (one injected wrapper, `scene_jitter` in `Shader.h`), so
  every bucket gets it whatever matrices it carries and the velocity buffer stays unjittered.
- Not for stereo or XR: those keep the MSAA path.

## Amendment (2026-10-18): DLSS super resolution over a D3D11 interop device
NVIDIA DLSS super resolution is the second temporal upscaler next to FSR 3.1 (the default), with FSR
3's placement, inputs and jitter (above). It is the first render pass that runs on another API: a
D3D11 device on the GL context's adapter evaluates it through the NGX D3D11 API on textures shared
with GL through WGL_NV_DX_interop2 (the HDR presenter already uses the same extension for its
swapchain). An upscaler vendor path is allowed (vendor-only *render* paths such as VRS are not); the
mode is offered only where it can run and falls back to FSR 3.1 elsewhere. The SDK is fetched at
build time, not vendored: its licence allows distributing it only inside the built application
(`third-party/nvidia-dlss/README.txt`). The release does not ship `nvngx_dlss.dll`; players bring
their own, placed next to `gk.exe` as with the disc image, or the runtime uses a copy in the NVIDIA
App's override cache. Not for stereo or XR.

## Amendment (2026-10-22): D3D12 as the DirectX container; FSR 4
D3D12 is the single DirectX container for every vendor feature (`D3d12Bridge.h`): one D3D12 device
at feature level 12_0 on the GL context's adapter (by LUID), one direct queue and fence. DLSS (after
its port) and FSR 4 run on it; the planned DXGI flip presenter uses its queue. D3D11 survives only
as the GL bridge, because WGL_NV_DX_interop2 accepts D3D10/11 devices only: a D3D11On12 device
wrapping the D3D12 device on the same queue. Shared textures are D3D12 committed resources wrapped
for D3D11On12 and registered with the interop; the interop lock, D3D11On12's acquire/release and its
flush onto the shared queue order the two APIs, so there is no second device and no cross-API fence
(verified on the AMD driver, `findings/2026-10-22-fsr4.md`). Without a D3D12 adapter the game has
FSR 3.1 on GL only and loses DLSS and FSR 4 (accepted). When no feature can use the device it is torn
down at startup.

AMD FSR 4 is the third temporal upscaler (engine mode 3) with FSR 3.1's placement, inputs and jitter.
It ships only as AMD-signed D3D12 DLLs (ffx-api, AMD FSR SDK 2.3.0, MIT): the headers are vendored,
the DLLs fetched at configure (hash-pinned) and shipped beside gk with their notice. The mode is
offered only where the runtime offers a provider of version 4 or later on this device (RDNA 4, RDNA 3
with a recent driver); elsewhere FSR 3.1 runs. Not for stereo or XR.

## Amendment (2026-10-23): bridge hand-overs stay on the WGL interop
GPU-side hand-overs through GL_EXT_memory_object_win32 / GL_EXT_semaphore_win32 were tried and do
not work on the AMD GL driver (imported D3D12 memory crashes at the first flush, semaphore signals
never reach the D3D12 fence, waits do not hold), so the interop's CPU-waiting lock and unlock stay
(`findings/2026-10-23-bridge-semaphores.md`).
