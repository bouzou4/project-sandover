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
