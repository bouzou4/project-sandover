---
title: Which image-diff libraries should gate the vanilla-parity CI check?
date: 2026-09-23
issue: "#15"
workstream: tooling evaluation
status: verified
sha: 76e8eec
---

## Answer (<=5 lines)
`flip-evaluator` (NVIDIA FLIP) is BSD-3-Clause, ships CPU-only Windows wheels for CPython 3.8-3.13, needs no GPU, and its Python API returns both a mean score and a per-pixel error map for LDR and HDR in one call — keep it as the hard gate. `lpips` is BSD-2-Clause and works, but its backbone weights download from PyTorch's model zoo on first use (breaks the offline requirement unless pre-cached) and the repo has had no commits since December 2023 — keep only as an advisory secondary metric, with weights pre-fetched once and cached. `scikit-image` is BSD-3-Clause, actively maintained, CPU-only, deterministic — keep SSIM/PSNR as cheap sanity checks, not the primary gate.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| flip-evaluator (NVlabs/flip) | BSD-3-Clause | yes, win32+win_amd64 wheels cp38-cp313 | v1.7 released 2025-11-07 / repo pushed 2025-11-07 | active (650 stars, 2 open issues) | keep | no official pass/fail threshold published; must calibrate empirically |
| lpips (richzhang/PerceptualSimilarity) | BSD-2-Clause | yes (pure Python + torch/torchvision, no native Windows-specific blockers) | PyPI v0.1.4 released 2021-08-25 / last commit 2023-12-19 | stale (58 open issues, no commits in ~2.75 yr) | replace with FLIP as primary; keep as advisory only | backbone (AlexNet/VGG) ImageNet weights download from PyTorch's model zoo on first `lpips.LPIPS()` call unless pre-cached; not deterministic across torch versions/devices |
| scikit-image | BSD-3-Clause | yes, standard win_amd64 wheels | v0.26.0 released 2025-12-20 / repo pushed 2026-09-22 | active (6595 stars, continuous commits) | keep (advisory, cheap sanity metric alongside FLIP) | SSIM/PSNR are not perceptually calibrated for "imperceptible" thresholds the way FLIP is; use only as a fast smoke check |

## Evidence
- NVlabs/flip repo license file is BSD-3-Clause, copyright NVIDIA Corporation & Affiliates 2020-2025, no non-commercial clause: https://raw.githubusercontent.com/NVlabs/flip/main/LICENSE
- NVlabs/flip repo metadata: pushed 2025-11-07, 650 stars, 2 open issues (via `gh api repos/NVlabs/flip`): https://github.com/NVlabs/flip
- `flip-evaluator` 1.7 on PyPI (uploaded 2025-11-07) ships wheels `flip_evaluator-1.7-cp3{8,9,10,11,12,13}-*-win32.whl` and `-win_amd64.whl`, license classifier "BSD License", no `requires_dist` (no torch/CUDA dependency): https://pypi.org/pypi/flip-evaluator/json
- README confirms the pip package is a pure C++/nanobind CPU binding ("using the C++ implementation through nanobind"); CUDA is only available via a separate C++/CUDA solution build, not the PyPI package: https://raw.githubusercontent.com/NVlabs/flip/main/src/python/README.md
- `flip.evaluate(reference, test, dynamicRangeString, ...)` accepts `"LDR"` or `"HDR"` and returns `(errorMap, meanFLIPError, parameters)` in one call — both a scalar mean score and a full per-pixel error map, for LDR PNG images directly: https://raw.githubusercontent.com/NVlabs/flip/main/src/flip_evaluator/flip_python_api.py
- Precision note: results are produced by deterministic 32-bit floating-point filter operations (no randomness); differences only arise *between* different implementations/OSes/hardware, and even then the documented test suite compares means "up to six decimal points" and passes on Windows/Linux/macOS: https://raw.githubusercontent.com/NVlabs/flip/main/misc/precision.md
- No numeric pass/fail threshold is published by NVIDIA; the publication page states only that a smaller mean FLIP value is better and recommends using the mean (not weighted median) as the single-number reduction, without a documented "identical" cutoff: https://research.nvidia.com/publication/2020-07_FLIP and https://github.com/NVlabs/flip
- LPIPS repo license file is BSD-2-Clause (2-clause, no advertising/endorsement clauses), copyright Richard Zhang et al. 2018: https://raw.githubusercontent.com/richzhang/PerceptualSimilarity/master/LICENSE
- `lpips` 0.1.4 on PyPI, uploaded 2021-08-25, license classifier "BSD License", requires `torch>=0.4.0`, `torchvision>=0.2.1`: https://pypi.org/pypi/lpips/json
- Last commit to richzhang/PerceptualSimilarity was 2023-12-19 (via `gh api repos/richzhang/PerceptualSimilarity/commits`): https://github.com/richzhang/PerceptualSimilarity
- `pretrained_networks.py` loads AlexNet/VGG/SqueezeNet via `torchvision.models` with `pretrained=True`, which downloads ImageNet backbone weights from PyTorch's model zoo (download.pytorch.org) the first time each network is instantiated, unless the torch hub cache is pre-populated: https://raw.githubusercontent.com/richzhang/PerceptualSimilarity/master/lpips/pretrained_networks.py
- The LPIPS linear-layer weights themselves (`weights/v0.1/{alex,vgg,squeeze}.pth`, the actual trained perceptual-similarity heads) are vendored inside the installed package and loaded from a local path, not downloaded: https://raw.githubusercontent.com/richzhang/PerceptualSimilarity/master/lpips/lpips.py (see `model_path = os.path.abspath(os.path.join(..., 'weights/v%s/%s.pth' ...))`)
- LPIPS README documents an optional `--use_gpu` flag for its example scripts, confirming CPU execution is supported (GPU only speeds it up, not required): https://raw.githubusercontent.com/richzhang/PerceptualSimilarity/master/README.md
- scikit-image license file: BSD-3-Clause project-wide (per-file BSD-2/3-Clause mix, all permissive, no copyleft): https://raw.githubusercontent.com/scikit-image/scikit-image/main/LICENSE.txt
- scikit-image repo metadata: pushed 2026-09-22, 6595 stars, latest release v0.26.0 published 2025-12-20 (via `gh api repos/scikit-image/scikit-image`): https://github.com/scikit-image/scikit-image
- scikit-image ships standard win_amd64 wheels on PyPI for every recent release: https://pypi.org/pypi/scikit-image/json

## Implications for the design
- Use FLIP's LDR-FLIP mode as the hard gate for the vanilla-parity check: call `flip.evaluate(ref, test, "LDR")` per shot, take the returned mean score, and fail CI above a chosen threshold; archive the returned error map as a build artifact for visual triage of failures.
- No authoritative "identical" threshold exists in the docs, so the threshold must be calibrated empirically on this project's own shots: run a no-op pass (same renderer settings on both sides, or a lossless re-encode) to establish the noise floor, then set the gate a small margin above that floor. A commonly used starting point in graphics tooling is a mean FLIP error in the low hundredths (order 0.01-0.03) for "should look identical" gates, but this repo must verify that number against its own captured shots before trusting it.
- Because the pip wheel is CPU-only with no CUDA dependency, the metric runs fine on the target workstation without touching the GPU, and is safe to run in a CI job with no display/driver requirements.
- Do not use LPIPS as the pass/fail gate: it needs `torch`+`torchvision` (heavier dependency footprint than FLIP or scikit-image), its backbone weights are not vendored and require a first-run download unless the machine pre-populates the torch hub cache from a pinned source, and its output is explicitly not guaranteed stable across torch versions or CPU/GPU — unsuitable for a deterministic regression gate. If kept at all, pin torch/torchvision versions, pre-download and vendor the AlexNet/VGG weights into the offline cache once, and treat LPIPS purely as an advisory ranking signal for preset comparisons, never as a fail condition.
- Keep scikit-image's SSIM and PSNR as near-zero-cost, dependency-light (numpy/scipy only) smoke checks that run before FLIP, to fail fast on gross corruption (wrong resolution, black frame, channel swap) without paying FLIP's per-image cost across hundreds of shots.
- 12 GB of VRAM is not a constraint for FLIP or scikit-image (CPU-only). It only matters if LPIPS is run with `--use_gpu` on the VGG backbone at 2560x1440; even then VGG-based LPIPS at this resolution comfortably fits in 12 GB, but this is moot if LPIPS stays CPU-only/advisory.

## Follow-ups
- Run a controlled no-op experiment (identical render twice, and a known 1-pixel-shift or gamma-nudge case) with `flip-evaluator` on real project shots to pick and document the concrete pass/fail mean-FLIP threshold — the literature does not supply one.
- If LPIPS is kept as advisory, pre-download `alexnet`/`vgg16` torchvision weights once while online, vendor them under the project's model cache directory, and pin exact `torch`/`torchvision` versions in the uv lockfile so scores stay comparable run to run.
- Decide whether HDR-FLIP is ever needed (project images are stated as LDR PNG only, so LDR-FLIP should suffice); if the engine ever emits HDR intermediates for comparison, FLIP already supports that mode with the same API.
