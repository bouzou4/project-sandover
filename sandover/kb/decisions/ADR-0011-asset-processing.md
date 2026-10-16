# ADR-0011: Offline asset (texture) processing pipeline
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The texture pipeline needs a scriptable, parameterizable runner for upscaling, normal-map
derivation, and (eventually) material channel splitting, run entirely offline over
thousands of small textures. The tooling evaluation covered ComfyUI vs chaiNNer as runners, and spandrel,
DeepBump, Marigold-IID, RGB<->X, and Material Palette as models.

## Options (with cost/risk)
**ComfyUI HTTP API, primary (chosen).** GPL-3.0 as an external process (fine — no GPL code
enters the engine), its `/prompt` API takes the full workflow JSON per call so every node
input is parameterizable from a script. Risk: custom nodes can silently auto-download
models; mitigate by pre-populating `models/` and avoiding fetch-happy nodes. Must disable
Comfy Desktop's telemetry toggle explicitly (not off by default on all builds).
**chaiNNer CLI, acceptable secondary.** The original specification's original premise — that its CLI can't
be parameterized — is stale: `--override` now supports Text/Number/File/Directory inputs.
Dropdown/checkbox inputs still can't be overridden and CLI mode remains labeled
experimental, so it stays secondary to ComfyUI rather than replacing it.
**RGB<->X — dropped.** Adobe Research License limits use to "academic research and
teaching," excludes anything with commercial gain — a gray area even for non-commercial
personal use — and its authors state a 12 GB VRAM minimum with no headroom on the target
card.
**DeepBump, Marigold-IID, Material Palette, spandrel-loaded upscalers — candidates.**
spandrel (MIT loader) is safe; every upscale weight is its own license decision via
OpenModelDB. DeepBump (GPL, ONNX, CPU-capable) is a clean normal-map keep. Marigold-IID
(openrail++) and Material Palette (per-material LoRA training, not batch inference) are
deferred pending a throughput pilot — license notes in
`kb/findings/2026-09-23-texture-pipeline.md`.

## Decision
ComfyUI's HTTP API is the primary runner, driven by scripts editing per-run workflow JSON,
run fully offline with a pre-populated model directory and telemetry off. chaiNNer's CLI
is acceptable for hand-built chains outside dropdown-parameterized nodes. RGB<->X is
dropped outright. DeepBump ships now; Marigold-IID, Material Palette and specific upscaler
weights remain candidates pending a per-model license log and a throughput pilot.

## Consequences
- Every checkpoint gets a license entry before use — no bundled assumption from
  spandrel's own MIT status. Source: `kb/findings/2026-09-23-texture-pipeline.md`.
