---
title: SSAO reference implementation for a fragment-only GL 4.1 pipeline
date: 2026-09-23
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
## Answer (<=5 lines)
McGuire SAO (G3D) is BSD-3-Clause, already a single fragment-shader pass, and directly parameterized by a world-space radius — it is the best starting point. HBAO+ is proprietary (NVIDIA 1-Way Commercial source license) and must not be used. XeGTAO is MIT but written as HLSL compute; a fragment-only GLSL port is feasible but nontrivial (depth-MIP prefilter and denoise passes need to move to fullscreen fragment passes). Three.js's GTAOShader.js (MIT, part of three.js) is already a fragment-shader GTAO with a world-space radius option and is a lower-effort alternative to porting XeGTAO from scratch.

## Picks
| reference | license (SPDX) | fragment-only feasible | source available | verdict | gotcha |
|---|---|---|---|---|---|
| McGuire SAO (G3D AmbientOcclusion_AO.pix) | BSD-3-Clause | yes, already fragment-shader GLSL | yes | keep | shader file carries two stacked BSD headers (NVIDIA 2011-2012 + McGuire/G3D 2000-2019) — both must be retained verbatim in the ported file |
| HBAO/HBAO+ (NVIDIA GameWorks) | Nvidia Source Code License, 1-Way Commercial (proprietary, not OSI-approved) | n/a | source on GitHub but under restrictive EULA | replace with McGuire SAO | not usable in a codebase that may contribute back to a permissive upstream; "1-way" grant runs only to NVIDIA |
| XeGTAO (Intel GameTechDev) | MIT | partial — algorithm portable, compute plumbing is not | yes | defer | depth-MIP prefilter uses `groupshared`, `GroupMemoryBarrierWithGroupSync`, and `RWTexture` MIP writes across an 8x8 thread group; GTAO/denoise passes use `numthreads` + RW targets — none of this exists in GLSL 4.10 fragment stage |
| Three.js GTAOShader.js | MIT | yes, already fragment-shader GLSL (WebGL) | yes | keep as secondary/reference | derived in part from Asylum Tutorials techniques, whose own repo carries no license (GitHub reports no detected license) — use the three.js file itself (MIT, clean provenance) rather than the original Asylum Tutorials source |
| Godot GTAO/SSAO (ssao.glsl) | MIT | yes, fragment/compute-hybrid GLSL in Godot's renderer, portable | yes | defer | tightly coupled to Godot's renderer conventions; usable as a secondary reference only, not a drop-in file |
| Blender EEVEE AO | GPL-2.0-or-later / GPL-3.0-or-later | n/a | yes | replace with McGuire SAO | GPL source — copying or closely deriving from it would require the fork (and any future upstream contribution) to be GPL-compatible; do not use as a code source, description-only reference is fine |
| ASSAO (Intel GameTechDev, archived) | MIT | no (as shipped) | yes | defer | DX11 HLSL, multi-pass adaptive pipeline with its own compute/graphics split; repo is archived, no longer maintained; porting effort likely exceeds a straight SAO or GTAO port |
| CryEngine-derived SSAO | CRYENGINE EULA (proprietary, not open source) | n/a | source visible under EULA | replace with McGuire SAO | commercial engine license, not usable as a code source |

## Evidence
- G3D core license is Modified BSD, copyright Morgan McGuire 2000-2014/2019: https://raw.githubusercontent.com/RomkoSI/G3D/master/license.txt
- The SAO reference shader (`AmbientOcclusion_AO.pix`, G3D10 data-files/shader/AmbientOcclusion) states "Open Source under the 'BSD' license: http://www.opensource.org/licenses/bsd-license.php", carries "Copyright (c) 2011-2012, NVIDIA. All rights reserved." plus a separate "G3D Innovation Engine ... Copyright 2000-2019, Morgan McGuire. All rights reserved. Available under the BSD License" block, and cites both the SAO (HPG 2012) and Alchemy AO (HPG 2011) papers as the algorithm's origin: https://sourceforge.net/p/g3d/code/HEAD/tree/G3D10/data-files/shader/AmbientOcclusion/AmbientOcclusion_AO.pix
- HBAO+ repo LICENSE.txt is the "Nvidia Source Code License (1-Way Commercial)" — a royalty-free grant back to NVIDIA, not a standard permissive OSS license: https://raw.githubusercontent.com/NVIDIAGameWorks/HBAOPlus/master/LICENSE.txt and repo listing https://github.com/NVIDIAGameWorks/HBAOPlus (GitHub API reports `license.spdx_id: NOASSERTION`, confirming it is not an OSI-standard license)
- XeGTAO repo is MIT (GitHub API: `license.spdx_id: MIT`, last pushed 2024-04-22, 899 stars): https://github.com/GameTechDev/XeGTAO and https://raw.githubusercontent.com/GameTechDev/XeGTAO/master/LICENSE (Intel Corporation, 2016-2021)
- XeGTAO's depth prefilter (`XeGTAO_PrefilterDepths16x16` in XeGTAO.hlsli) declares `groupshared lpfloat g_scratchDepths[8][8]` and calls `GroupMemoryBarrierWithGroupSync()` three times while writing MIP levels 0-4 via `outDepth0`..`outDepth4` RW targets: https://raw.githubusercontent.com/GameTechDev/XeGTAO/master/Source/Rendering/Shaders/XeGTAO.hlsli
- The main GTAO/denoise compute entry points (`CSPrefilterDepths16x16`, `CSGTAOLow/Medium/High/Ultra`, `CSDenoisePass`, `CSDenoiseLastPass`, `CSGenerateNormals`) all declare `[numthreads(...)]` and write through `RWTexture` targets (`g_outWorkingDepthMIP*`, `g_outWorkingAOTerm`, `g_outWorkingEdges`, `g_outFinalAOTerm`, `g_outNormalmap`); no wave intrinsics are used: https://raw.githubusercontent.com/GameTechDev/XeGTAO/master/Source/Rendering/Shaders/vaGTAO.hlsl
- XeGTAO exposes `Radius`: "World (view) space size of the occlusion sphere", default 0.5, confirming it is designed around a world-space radius knob: https://github.com/GameTechDev/XeGTAO/blob/master/Source/Rendering/Effects/vaGTAO.h
- three.js is MIT-licensed (GitHub API: `license.spdx_id: MIT`): https://github.com/mrdoob/three.js
- `examples/jsm/shaders/GTAOShader.js` in three.js is a single fragment-shader pass GTAO implementation with a world/view-space `radius` parameter and an optional `SCREEN_SPACE_RADIUS` toggle, citing Jimenez et al.'s GTAO paper and Mayaux's HBIL notes: https://github.com/mrdoob/three.js/blob/master/examples/jsm/shaders/GTAOShader.js
- Asylum_Tutorials (an earlier fragment-shader GTAO reference some ports cite) has no detected license on GitHub (`license: null` / `NOASSERTION` via API), so it should not be used as a code source even though its techniques are referenced elsewhere: https://github.com/asylum2010/Asylum_Tutorials
- Godot is MIT-licensed; a GTAO variant of its SSAO shader (proposed/implemented alongside the existing SSAO) is discussed here: https://github.com/godotengine/godot-proposals/issues/3223 and https://godotengine.org/license/
- Blender's source is GPL-2.0-or-later (GPL-3.0-or-later for binary distribution): https://www.blender.org/about/license/ — GPL is not compatible with a codebase that intends to keep pieces contributable to a permissively (ISC) licensed upstream
- ASSAO (GameTechDev/ASSAO) is MIT but archived (GitHub API: `archived: true`, last pushed 2023-01-03): https://github.com/GameTechDev/ASSAO
- CRYENGINE's source is distributed under the CRYENGINE EULA, not an open-source license: https://www.cryengine.com/ce-terms (referenced from repo listings, e.g. https://github.com/MergHQ/CRYENGINE)

## Implications for the design
- Port McGuire's SAO shader (`AmbientOcclusion_AO.pix`) as the primary AO pass: it is already single-pass fragment GLSL, reconstructs normals from depth, and takes a world-space radius plus the projection matrix — matching the stated inputs exactly.
- The ported shader file must retain both BSD headers verbatim (NVIDIA 2011-2012 copyright/disclaimer and the G3D/McGuire 2000-2019 copyright/disclaimer) per BSD-3-Clause's source-redistribution clause; add a comment citing the HPG 2012 SAO paper and HPG 2011 Alchemy AO paper as good practice even though the header does not impose a mandatory citation clause beyond retaining the copyright/license text.
- Do not use HBAO+, CryEngine SSAO, or Blender EEVEE AO as code sources under any circumstances — proprietary/GPL terms would taint the private fork and specifically block any future contribution of generic pieces back to the ISC-licensed upstream.
- If SAO's obscurance model proves visually insufficient later, Three.js's `GTAOShader.js` (MIT, fragment-only, world-space radius) is a lower-risk upgrade path than porting XeGTAO, since it avoids the compute-to-fragment rewrite entirely.
- A full XeGTAO port is feasible but should be scoped as a larger follow-on: replace the 8x8 groupshared depth-MIP prefilter with 4-5 sequential fullscreen fragment passes (each writing one MIP level of a depth pyramid via `textureLod`/manual downsample), replace the single fused GTAO+denoise compute dispatch with two fragment passes (AO term, then a fragment-based denoise/bilateral blur reading neighbor AO+edges), and drop the normal-generation compute pass in favor of the depth-based reconstruction already used for SAO. Rough estimate: 3-5 days for a working port plus tuning, versus under a day to integrate SAO or the three.js GTAO shader directly.

## Follow-ups
- Prototype SAO with the existing depth texture and projection matrix; tune the world-space radius small enough to avoid double-darkening against the baked vertex-color AO already in the lighting.
- If contact-AO quality from SAO is insufficient, evaluate Three.js's GTAOShader.js as a drop-in fragment alternative before considering a XeGTAO port.
- Confirm with the maintainers whether any future upstream contribution policy needs an explicit note that the AO pass originates from BSD-3-Clause G3D code (attribution already satisfied by retaining the header, but flag for changelog/NOTICE tracking).
