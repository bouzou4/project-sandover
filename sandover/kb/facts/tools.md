# Tools ledger

One line per "buy, don't build" need from the tooling evaluation. `status` is `verified`
unless noted. Rows with their own ADR are marked; everything else kept its default pick
with no ADR needed.

| need | pick | status | source note | date |
|---|---|---|---|---|
| Python env/packages | uv | verified | kb/findings/2026-09-23-python-infra.md | 2026-09-23 |
| Build/game locks | filelock (>=3.20.3) | verified | kb/findings/2026-09-23-python-infra.md | 2026-09-23 |
| KB site (ADR-0003) | MkDocs Material | verified | kb/findings/2026-09-23-python-infra.md | 2026-09-23 |
| Notebooks | marimo | verified | kb/findings/2026-09-23-notebooks.md | 2026-09-23 |
| Unbake fit, outer | scipy.optimize.least_squares | verified | kb/findings/2026-09-23-unbake-fit.md | 2026-09-23 |
| Unbake fit, inner | scipy.optimize.lsq_linear / nnls | verified | kb/findings/2026-09-23-unbake-fit.md | 2026-09-23 |
| Unbake batching fallback | PyTorch (GPU) | verified | kb/findings/2026-09-23-unbake-fit.md | 2026-09-23 |
| Point-light clustering | scikit-learn HDBSCAN | verified | kb/findings/2026-09-23-unbake-fit.md | 2026-09-23 |
| glTF color-set I/O (ADR-0010) | pygltflib | verified | kb/findings/2026-09-23-gltf-io.md | 2026-09-23 |
| Mesh geometry helpers | trimesh (geometry only) | verified | kb/findings/2026-09-23-gltf-io.md | 2026-09-23 |
| Parity gate (ADR-0008) | flip-evaluator (NVIDIA FLIP) | verified | kb/findings/2026-09-23-image-metrics.md | 2026-09-23 |
| Perceptual advisory metric (ADR-0008) | lpips | verified | kb/findings/2026-09-23-image-metrics.md | 2026-09-23 |
| Cheap pre-check (ADR-0008) | scikit-image SSIM/PSNR | verified | kb/findings/2026-09-23-image-metrics.md | 2026-09-23 |
| Preference loop | BoTorch PairwiseGP + qEUBO (round 3+) | verified | kb/findings/2026-09-23-preference-bo.md | 2026-09-23 |
| Review UI | Gradio (hardened) | verified | kb/findings/2026-09-23-picker-ui.md | 2026-09-23 |
| Pipeline observability | Rerun | verified | kb/findings/2026-09-23-observability.md | 2026-09-23 |
| Pixel peeping / A-B | tev | verified | kb/findings/2026-09-23-observability.md | 2026-09-23 |
| Texture dataset browser | FiftyOne | verified | kb/findings/2026-09-23-dataset-review.md | 2026-09-23 |
| Visual regression report (ADR-0006) | hand-rolled Jinja2 + static HTML slider | verified | kb/findings/2026-09-23-dataset-review.md | 2026-09-23 |
| Contact sheets | Pillow | verified | kb/findings/2026-09-23-contact-sheets.md | 2026-09-23 |
| GPU capture/inspect (ADR-0007) | RenderDoc | verified | kb/findings/2026-09-23-gpu-debug.md | 2026-09-23 |
| GPU per-pass timing (ADR-0007) | in-engine glQueryCounter + ImGui profiler | verified | kb/findings/2026-09-23-gpu-debug.md | 2026-09-23 |
| SSAO reference (ADR-0009) | McGuire SAO (G3D, BSD-3) | verified | kb/findings/2026-09-23-ssao-reference.md | 2026-09-23 |
| VR loader (ADR-0004) | OpenXR-SDK (Khronos) | verified | kb/findings/2026-09-23-vr-stack.md | 2026-09-23 |
| VR runtime/GL binding (ADR-0004) | SteamVR + XR_KHR_opengl_enable | verified | kb/findings/2026-09-23-vr-stack.md | 2026-09-23 |
| Texture pipeline runner (ADR-0011) | ComfyUI HTTP API | verified | kb/findings/2026-09-23-texture-pipeline.md | 2026-09-23 |
| Upscaler loading (ADR-0011) | spandrel (per-weight license check) | verified | kb/findings/2026-09-23-texture-pipeline.md | 2026-09-23 |
| Normal maps (ADR-0011) | DeepBump | verified | kb/findings/2026-09-23-texture-pipeline.md | 2026-09-23 |
| Build cache (ADR-0012) | sccache, opt-in via SANDOVER_SCCACHE=1 | verified | kb/findings/2026-09-23-tool-evaluation-summary.md | 2026-09-23 |
| Gaze/eye tracking (ADR-0004) | none — dropped from scope | refuted | kb/findings/2026-09-23-vr-stack.md | 2026-09-23 |
