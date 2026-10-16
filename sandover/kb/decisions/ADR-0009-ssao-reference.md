# ADR-0009: SSAO reference implementation and license
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The AO pass needs a reference implementation compatible with a fragment-only GLSL
pipeline (shader source pinned to `#version 410 core` for portability) and a world-space
radius parameter. The tooling evaluation surveyed McGuire SAO, HBAO/HBAO+, XeGTAO, three.js's GTAOShader.js,
Godot's SSAO, Blender EEVEE AO, ASSAO, and CryEngine's SSAO for both technical fit and
license.

## Options (with cost/risk)
**McGuire SAO (G3D `AmbientOcclusion_AO.pix`), BSD-3-Clause (chosen).** Already a single
fragment-shader GLSL pass, reconstructs normals from depth, takes a world-space radius and
the projection matrix directly. Cost: the ported file must retain both stacked BSD
headers (NVIDIA 2011-2012 + McGuire/G3D 2000-2019) verbatim. Risk: low.
**XeGTAO (Intel), MIT.** Technically strong (GTAO) but written as HLSL compute — depth-MIP
prefilter and denoise passes use `groupshared`/`RWTexture` constructs with no fragment-
stage equivalent. Estimated 3-5 days to port versus under a day for SAO; deferred as a
future upgrade path.
**three.js GTAOShader.js, MIT.** Already fragment-shader GLSL with a world-space radius;
lower-risk upgrade path than a XeGTAO port if SAO proves insufficient. Kept as secondary
reference.
**HBAO/HBAO+, CryEngine SSAO, Blender EEVEE AO — excluded.** Proprietary NVIDIA
1-Way-Commercial license, proprietary CryEngine EULA, and GPL respectively; none are
usable in a codebase that stays contributable to a permissively (ISC) licensed upstream.

## Decision
Port McGuire's SAO as the primary AO pass: fragment-only, world-space radius, with a
bilateral blur pass for denoise. Both BSD headers are retained verbatim in the ported
file. HBAO+, CryEngine SSAO, and Blender EEVEE AO are hard excludes from any code source.
Three.js's GTAOShader.js is the fallback upgrade path if SAO's obscurance model is
visually insufficient; a full XeGTAO port is a larger, separately-scoped follow-on.

## Consequences
- Tune the world-space radius to avoid double-darkening against the baked vertex-color AO
  already in the lighting.
- Source: `kb/findings/2026-09-23-ssao-reference.md`.
