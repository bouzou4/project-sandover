# ADR-0007: GPU debugging and profiling
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The SSAO/lighting work needs both GPU frame capture (correctness debugging) and per-pass
timing (AO cost budgeting) on an OpenGL 4.3 core context. The tooling evaluation found that NVIDIA Nsight
Graphics's GPU Trace / Frame Profiler — its counter and timing engine — has never
supported OpenGL; only its legacy "OpenGL Frame Debugger" remains, and even that tool's
Shader Profiler is being removed in the next NVIDIA release.

## Options (with cost/risk)
**RenderDoc (chosen, primary).** MIT, actively maintained, full GL 3.2-4.6 core capture,
scriptable Python API, per-draw GPU timings via "calculate timings". Risk: GL shader
step-through debugging needs `ARB_compute_shader` + `ARB_shader_storage_buffer_object` on
the replay driver — capture always works regardless; step-through needs a one-time local
check.
**Nsight Graphics.** Cost/risk: GPU Trace/Frame Profiler never covered OpenGL, so it
cannot be the AO-pass budgeting mechanism; kept installed only as a fallback for basic GL
draw-call/state inspection via its legacy Frame Debugger, not for profiling.
**In-engine `glQueryCounter(GL_TIMESTAMP)` + existing Dear ImGui profiler (chosen,
budgeting).** Cost: engine-side instrumentation work. Risk: low — this is the only tool
here with a first-class GL profiling story, since neither RenderDoc nor Nsight offers one.

## Decision
RenderDoc is primary for capture/inspect/shader-edit. Per-pass GPU cost is measured with
in-engine `glQueryCounter(GL_TIMESTAMP)` scopes surfaced in the existing ImGui profiler
overlay, cross-checked periodically against RenderDoc's per-action timings. Nsight
Graphics is dropped as a profiling tool — it has no OpenGL GPU trace path — and kept only
as an optional secondary GL state inspector.

## Consequences
- No time is budgeted to integrate Nsight Graphics into the capture/profiling workflow.
- Confirm `ARB_compute_shader` + `ARB_shader_storage_buffer_object` are present on the
  target GPU/driver once, before relying on RenderDoc's GL shader step-through.
- Source: `kb/findings/2026-09-23-gpu-debug.md`.
