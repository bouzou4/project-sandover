# ADR-0008: Image metrics for vanilla-parity and quality gates
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
Vanilla-parity CI needs a hard pass/fail gate; preset comparisons want a perceptual
ranking signal; both want a cheap pre-filter to catch gross corruption before paying for
the expensive metric. The tooling evaluation covered NVIDIA FLIP (`flip-evaluator`), LPIPS, and
scikit-image's SSIM/PSNR. No published pass/fail FLIP threshold exists anywhere in the
literature — it must be calibrated on this project's own captures.

## Options (with cost/risk)
**flip-evaluator (NVIDIA FLIP), hard gate (chosen).** BSD-3-Clause, CPU-only Windows
wheels, no CUDA dependency, returns both a mean score and a per-pixel error map in one
call. Risk: no off-the-shelf threshold — must calibrate via a repeated identical capture
to find the noise floor before trusting a number.
**LPIPS, advisory only (chosen).** BSD-2-Clause, works, but its backbone weights download
from PyTorch's model zoo on first use (breaks offline-first-run unless pre-cached) and the
repo has had no commits since December 2023. Not deterministic across torch
versions/devices — unfit for a hard gate.
**scikit-image SSIM/PSNR, cheap pre-check (chosen).** BSD-3-Clause, actively maintained,
CPU-only, deterministic, near-zero cost — but not perceptually calibrated for
"imperceptible" thresholds the way FLIP is.

## Decision
FLIP's LDR mean is the sole hard gate for vanilla-parity CI, with the pass/fail threshold
calibrated empirically on this project's own shot set (no published number is trusted).
LPIPS is advisory only and never a fail condition; its backbone weights must be pre-cached
once, online, before relying on it offline. SSIM/PSNR via scikit-image run first as a
cheap sanity check (wrong resolution, black frame, channel swap) ahead of the FLIP pass.

## Consequences
- Run a no-op calibration pass (identical capture twice) before writing any threshold into
  CI; archive FLIP error maps as build artifacts for failure triage.
- If LPIPS is kept, pin `torch`/`torchvision` versions and vendor the AlexNet/VGG weights
  into the offline model cache once.
- Source: `kb/findings/2026-09-23-image-metrics.md`.
