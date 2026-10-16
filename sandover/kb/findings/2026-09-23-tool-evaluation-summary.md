---
title: Tooling evaluation — final picks
date: 2026-09-23
issue: "#15"
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
> Editorial note (2026-09-23, post-ADR-0004): the VR scope decision dropped eye tracking,
> gaze and PSVR2Toolkit entirely (stereo 3D via OpenXR + SteamVR only). The gaze rows below
> are kept as a record of the evaluation; treat them as refuted/moot, not as a plan item.
## Answer (<=5 lines)
Every row of the "buy, don't build" table was checked for license, Windows support, maintenance and fit.
Most picks hold. Four change: reg-cli is dropped for a hand-rolled FLIP-driven HTML slider report; Nsight
Graphics is demoted to fallback (GPU Trace has never supported OpenGL); RGB<->X is dropped on license;
ImageMagick is demoted to a manual escape hatch. Three carry sunset or licensing risk needing a decision:
MkDocs Material (feature-frozen, EOL 2026-11), PSVR2Toolkit (MIT file vs non-commercial README), Gradio
(telemetry and a font CDN on by default).

## Picks
| need | pick | verdict | alternative | note |
|---|---|---|---|---|
| Python env | uv | keep | — | kb/findings/2026-09-23-python-infra.md |
| Locks | filelock | keep | portalocker | kb/findings/2026-09-23-python-infra.md |
| KB site | MkDocs Material | keep, revisit before 2026-11 | Zensical (successor) | kb/findings/2026-09-23-python-infra.md |
| Notebooks | marimo | keep | Jupyter via jupytext py:marimo | kb/findings/2026-09-23-notebooks.md |
| Unbake fit (outer) | scipy.optimize.least_squares | keep | — | kb/findings/2026-09-23-unbake-fit.md |
| Unbake fit (inner) | scipy.optimize.lsq_linear / nnls | keep (added) | — | kb/findings/2026-09-23-unbake-fit.md |
| Unbake batching | PyTorch | keep (fallback only) | — | kb/findings/2026-09-23-unbake-fit.md |
| Point-light clustering | scikit-learn HDBSCAN | keep | DBSCAN, MeanShift | kb/findings/2026-09-23-unbake-fit.md |
| glTF colour-set I/O | pygltflib | keep | gltflib | kb/findings/2026-09-23-gltf-io.md |
| Mesh utilities | trimesh | keep (geometry only) | numpy direct | kb/findings/2026-09-23-gltf-io.md |
| Parity metric | flip-evaluator | keep (hard gate) | — | kb/findings/2026-09-23-image-metrics.md |
| Perceptual metric | lpips | keep (advisory only) | — | kb/findings/2026-09-23-image-metrics.md |
| Cheap pre-check | scikit-image SSIM/PSNR | keep | — | kb/findings/2026-09-23-image-metrics.md |
| Preference loop | BoTorch PairwiseGP + qEUBO | keep (from round 3) | winner-jitter baseline | kb/findings/2026-09-23-preference-bo.md |
| Gallery interaction design | Sequential Gallery (Koyama 2020) | keep (reference only) | — | kb/findings/2026-09-23-preference-bo.md |
| Review UI | Gradio | keep (must be hardened) | static HTML + http.server | kb/findings/2026-09-23-picker-ui.md |
| Pipeline observability | Rerun | keep | — | kb/findings/2026-09-23-observability.md |
| Pixel peeping / A-B | tev | keep | — | kb/findings/2026-09-23-observability.md |
| Texture dataset review | FiftyOne | keep | — | kb/findings/2026-09-23-dataset-review.md |
| Visual regression report | reg-cli | replace with Jinja2 + static HTML slider | odiff, pixelmatch, BackstopJS | kb/findings/2026-09-23-dataset-review.md |
| Contact sheets | Pillow | keep | ImageMagick (manual only) | kb/findings/2026-09-23-contact-sheets.md |
| GPU debugging | RenderDoc | keep (primary) | apitrace | kb/findings/2026-09-23-gpu-debug.md |
| GPU profiling | Nsight Graphics | replace with in-engine GL timestamp queries | Nsight for GL state inspection only | kb/findings/2026-09-23-gpu-debug.md |
| SSAO reference | McGuire SAO (G3D) | keep | three.js GTAOShader.js, XeGTAO | kb/findings/2026-09-23-ssao-reference.md |
| VR loader | OpenXR SDK | keep | — | kb/findings/2026-09-23-vr-stack.md |
| VR runtime / GL binding | SteamVR + XR_KHR_opengl_enable | keep | D3D11 interop via WGL_NV_DX_interop2 | kb/findings/2026-09-23-vr-stack.md |
| Gaze transport | XR_EXT_eye_gaze_interaction | dropped — out of scope, see ADR-0004 | PSVR2Toolkit native CAPI | kb/findings/2026-09-23-vr-stack.md |
| Gaze source | PSVR2Toolkit | dropped — out of scope, see ADR-0004 | — | kb/findings/2026-09-23-vr-stack.md |
| Texture pipeline runner | ComfyUI HTTP API | keep | chaiNNer CLI | kb/findings/2026-09-23-texture-pipeline.md |
| Upscaler loading | spandrel | keep (per-weight license check) | — | kb/findings/2026-09-23-texture-pipeline.md |
| Normal maps | DeepBump | keep | — | kb/findings/2026-09-23-texture-pipeline.md |
| Intrinsic decomposition | Marigold-IID | defer | — | kb/findings/2026-09-23-texture-pipeline.md |
| Intrinsic decomposition | RGB<->X | drop (license) | Marigold-IID | kb/findings/2026-09-23-texture-pipeline.md |
| Material extraction | Material Palette | defer | — | kb/findings/2026-09-23-texture-pipeline.md |

## Rows that changed, and why
- **reg-cli replaced.** Windows is excluded from reg-cli's own CI matrix (its docs point Windows users at
  WSL or git-bash), it is still shipping release candidates, and it would add a Node toolchain beside uv.
  The FLIP gate already produces the numbers a slider report needs.
- **Nsight Graphics demoted.** GPU Trace and the Frame Profiler have never covered OpenGL; only the legacy
  GL Frame Debugger remains and its Shader Profiler is removed in the next release. Per-pass AO cost must
  come from in-engine GL timestamp queries plus the existing ImGui profiler.
- **RGB<->X dropped.** Weights carry an Adobe Research license limited to academic research and teaching,
  and the authors state a 12 GB VRAM minimum, which leaves no headroom on the target card.
- **ImageMagick demoted.** Pillow is already a transitive dependency and needs no system package.
- **chaiNNer premise outdated.** Its CLI now has an override flag for text, number, file and directory
  inputs. Dropdown and checkbox inputs still cannot be overridden and CLI mode is still labelled
  experimental, so ComfyUI wins on parameterization — but not for the reason originally cited.

## Gotchas that must reach implementation
- **trimesh silently drops COLOR_1..COLOR_7.** Its glTF loader references only COLOR_0, and it merges
  vertices unless loading is unprocessed. Either would destroy the vertex-to-palette-entry mapping the
  unbake depends on. pygltflib must own all colour-set I/O.
- **No published FLIP pass/fail threshold exists.** Calibrate the noise floor with a repeated identical
  capture before hardcoding a parity number.
- **lpips downloads backbone weights on first use**, which breaks the offline-first-run requirement.
  Pre-cache them or leave it out of the default path.
- **Gradio phones home by default** (analytics ping, version check) and loads a font from a CDN. It needs
  analytics disabled by both environment variable and launch argument, share left off, a project-local
  temp directory, and a self-hosted font before it is airtight.
- **Rerun and FiftyOne both ship opt-out telemetry.** Logged content stays local either way, but both
  opt-outs belong in the project environment file.
- **RenderDoc's Python module must match the RenderDoc build exactly**, which conflicts with a uv-managed
  interpreter. Script it against the shipped Python, not the project environment.
- **tev is GPL-3.0.** Fine as an unmodified external binary driven over local TCP IPC; do not vendor or link.
- **ComfyUI and chaiNNer are both GPL-3.0**, including the ComfyUI frontend. Fine as separate processes
  driven over HTTP; no GPL code enters the engine fork.
- **ComfyUI custom nodes can silently auto-download models.** Pre-populate the model directory and avoid
  fetching nodes so that runs stay offline.
- **MkDocs Material is feature-frozen** since 2025-11 with end of life in 2026-11. All former Insiders
  features are now in the free edition, so it is complete today, but the KB needs a successor within a year.
- **PSVR2Toolkit ships an MIT license file but its README asks for non-commercial use only**, and its native
  client API is still changing shape. Moot: ADR-0004 drops gaze/PSVR2Toolkit from scope entirely.
- **uv's automatic torch backend selection is preview** and unsupported in add/sync/run. Pin the CUDA index
  explicitly in project configuration.
- **marimo's reactive runtime will re-run an expensive fit cell.** Gate long cells behind an explicit run
  button, or switch the runtime to lazy, and memoize with persistent caching.

## Proposed ADRs
- **ADR: glTF colour-set I/O split.** pygltflib owns all accessor and colour-set I/O; trimesh is used only
  for geometry on pre-extracted arrays, unprocessed, with no vertex merging.
- **ADR: parity metric and gate.** FLIP LDR mean is the only hard gate; SSIM/PSNR are a cheap pre-filter;
  LPIPS is advisory and never a fail condition. The threshold comes from empirical calibration.
- **ADR: unbake solver architecture.** Variable projection — an outer robust least-squares over the eight
  light-slot parameters wrapping a vectorized bounded linear solve per palette entry. PyTorch only if the
  CPU path is too slow.
- **ADR: visual regression report.** Hand-roll a static HTML before/after slider driven by FLIP output; no
  Node toolchain in this project.
- **ADR: SSAO reference and license.** Port McGuire SAO (BSD-3-Clause) with both original headers retained.
  HBAO+, CryEngine SSAO and Blender EEVEE AO are hard excludes.
- **ADR: GPU cost measurement.** In-engine GL timestamp query scopes plus the existing ImGui profiler are
  the budgeting mechanism; RenderDoc for correctness; Nsight only for GL state inspection.
- **ADR: VR graphics binding.** Target the OpenGL binding extension on SteamVR; keep the D3D11 interop path
  as a benchmarked contingency, not a required build.
- **ADR: gaze source.** Superseded before being written: ADR-0004 (VR scope) dropped eye tracking, gaze and
  PSVR2Toolkit entirely. No gaze ADR was needed.
- **ADR: texture pipeline runner.** ComfyUI's HTTP API, run offline with a pre-populated model directory and
  telemetry off. chaiNNer stays available for hand-built chains.
- **ADR: model licensing policy.** Every checkpoint carries a recorded license before use. RGB<->X excluded;
  Marigold-IID and Material Palette deferred pending a throughput pilot; upscaler weights checked against
  OpenModelDB's license field.
- **ADR: notebook tool.** marimo, with expensive cells gated and results persistently cached.
- **ADR: offline posture for local web tools.** One documented set of environment variables and launch flags
  disabling telemetry, CDN fetches and tunnels across Gradio, Rerun, FiftyOne and ComfyUI.

## Follow-ups
- Smoke-test embedding Rerun's viewer and FiftyOne's app inside marimo; no documented integration was found.
- Moot: gaze is out of scope per ADR-0004, so no follow-up is needed on the OpenXR gaze extension.
- Spot-check RenderDoc against the engine's MSAA depth-renderbuffer blit; the known black-MSAA quirk is
  documented for GLES, not desktop GL core.
- Calibrate the FLIP noise floor on a repeated identical capture before writing the parity threshold into CI.
- Re-check the KB site successor before 2026-11.
