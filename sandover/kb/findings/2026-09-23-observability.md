---
title: Rerun and tev for offline pipeline observability and pixel peeping
date: 2026-09-23
workstream: tooling evaluation
status: verified
sha: 76e8eec
---
## Answer (<=5 lines)
Rerun (Apache-2.0/MIT dual license) fits the run-timeline/mesh/scalar logging use case: Windows wheels ship on PyPI with the native viewer bundled, it runs fully offline, and per-vertex mesh colors plus scalar time series and blueprints are supported, but the 0.x API still takes breaking changes across minor versions so pin exactly. tev (GPL-3.0) fits A/B pixel peeping: official Windows .exe/.msi builds exist, it reads PNG and EXR, and the official `tevclient` PyPI package drives its TCP IPC protocol (127.0.0.1:14158) to push/update/close images from Python. Both keep, both offline, both maintained as of 2026-09-23.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| Rerun (rerun-sdk) | Apache-2.0 OR MIT (dual) | yes, wheels on PyPI, viewer bundled with Python package | v0.38.1 (2026-09-17) / last commit 2026-09-22 | active, ~11.5k stars, ~1265 open issues | keep | 0.x API has breaking changes across minor versions; pin exact version and re-check migration notes on upgrade; release builds phone home usage analytics by default (`rerun analytics disable` to opt out) though logged content stays local |
| tev | GPL-3.0 | yes, official .msi/.exe on GitHub releases | v2.14.0 (2026-08-23) / last commit 2026-09-11 | active but small, ~1.4k stars, 18 open issues | keep | GPL-3.0 is copyleft on tev's own source (fine since only used as an external, unmodified tool over IPC, not linked into the engine); official `tevclient` needs Python >=3.11 |

## Evidence
- Rerun repo license, activity, issues: SPDX `Apache-2.0`, last push 2026-09-22, 11484 stars, 1265 open issues — `gh api repos/rerun-io/rerun`.
- Rerun latest release tag `0.38.1`, published 2026-09-17 — `gh api repos/rerun-io/rerun/releases/latest`.
- Rerun is dual-licensed MIT OR Apache-2.0 for the open-source SDK/viewer; commercial "Rerun Hub" is a separate managed backend offering, not a license restriction on the OSS SDK: https://github.com/rerun-io/rerun/blob/main/LICENSE-MIT , https://github.com/rerun-io/rerun/blob/main/LICENSE-APACHE , https://rerun.io/docs/reference/about
- `pip install rerun-sdk` publishes wheels including Windows platforms on PyPI, and only the Python SDK ships bundled with the native Viewer (C++/Rust require a separate viewer install): https://pypi.org/project/rerun-sdk/ , https://github.com/rerun-io/rerun/blob/main/rerun_py/README.md
- Analytics opt-out via `rerun analytics disable`; debug builds have analytics off by default; first launch does not send data before the user has a chance to opt out; no PII is collected: https://docs.rs/re_analytics/latest/re_analytics/
- Mesh3D archetype supports an optional `vertex_colors` field, one color per vertex: https://rerun.io/docs/reference/types/archetypes/mesh3d
- Scalar time series use the `Scalars` archetype logged to an entity path, visualized via `SeriesLines`/`SeriesPoints` and `TimeSeriesView`: https://rerun.io/docs/reference/types/archetypes/scalars , https://rerun.io/docs/reference/types/archetypes/series_lines , https://rerun.io/docs/reference/types/views/time_series_view
- `.rrd` files can be larger than RAM because the Viewer catalog streams/loads chunks on demand; `rerun rrd optimize` rewrites files with fewer/larger chunks; the Viewer's store garbage-collects old rows once resident memory pressure is hit (eviction is irreversible short of re-logging): https://rerun.io/docs/changelog/upcoming/viewer-catalog-default , https://rerun.io/docs/howto/logging-and-ingestion/optimize-chunks , https://github.com/rerun-io/rerun/issues/5974
- Blueprints let a run open with a preconfigured layout via `rrb.Blueprint(...)` passed as `default_blueprint` to `init`/`connect_grpc`, or sent live with `rr.send_blueprint()`, and can be saved to a file for reuse: https://rerun.io/docs/concepts/blueprints , https://rerun.io/docs/howto/build-a-blueprint-programmatically
- Rerun explicitly states the API is still evolving with breaking changes expected pre-1.0, and only guarantees `.rrd` file-format compatibility across adjacent minor versions (e.g. 0.24 can open 0.23 files): https://rerun.io/docs/changelog/changeset-0-38 , https://github.com/rerun-io/rerun/blob/main/CHANGELOG.md
- tev repo license, activity, issues: SPDX `GPL-3.0`, last push 2026-09-11, 1438 stars, 18 open issues — `gh api repos/Tom94/tev`.
- tev latest release tag `v2.14.0`, published 2026-08-23 — `gh api repos/Tom94/tev/releases/latest`.
- tev license is GPLv3 (versions ≤1.30 were BSD-3-Clause); supported formats include PNG, JPEG, BMP, TIFF, EXR, JPEG XL, AVIF, HEIC, RAW, DICOM, FITS, QOI, DDS and more; IPC listens by default on `127.0.0.1:14158` with a length-prefixed binary packet protocol; documented operations include `OpenImage`, `ReloadImage`, `CreateImage`, `UpdateImage`, `CloseImage`, `VectorGraphics`: https://github.com/Tom94/tev/blob/master/README.md
- Windows releases ship both an installer (`tev-installer.msi`) and a portable executable (`tev.exe`), plus ARM variants: https://github.com/Tom94/tev/releases
- Official Python IPC client `tevclient` is published on PyPI (installable via `pip install tevclient`, requires Python >=3.11) and implements tev's IPC protocol over a TCP connection to open/update/reload/close images and draw overlays: https://pypi.org/project/tevclient/ , https://github.com/Tom94/tevclient

## Implications for the design
- Rerun: log captures as `Image`/`EncodedImage`, per-run scalar metrics as `Scalars` + `SeriesLines`/`SeriesPoints` on a `run` timeline, per-vertex fit residuals and recovered light directions as `Mesh3D` with `vertex_colors` (or a second mesh instance colored by residual), and the hour-of-day sweep as an additional timeline axis (`rr.set_time(...)`) so the Viewer's timeline scrubber covers all four data kinds in one `.rrd` per run.
- Ship a saved blueprint (Python `rrb.Blueprint(...)`, saved via `.save()`) alongside the pipeline so every teammate/run opens with the same panel layout (mesh view + scalar plots + image comparison) without manual re-arranging.
- Pin `rerun-sdk` to an exact version in the environment (uv lock) rather than a range, and treat `.rrd` files as tied to that SDK version; re-run `rerun rrd optimize` on artifact `.rrd` files before archiving if they accumulate many small chunks from per-frame logging.
- Run `rerun analytics disable` once on the target workstation to guarantee no outbound telemetry, even though logged image/mesh content itself never leaves the machine (analytics is usage-event telemetry only, separate from the data store).
- tev's GPL-3.0 only encumbers tev's own source; driving it as a standalone external process over the local TCP IPC socket does not pull GPL into the engine or pipeline code — keep tev as an unmodified prebuilt binary and integrate via `tevclient` calls only.
- `tevclient` requires Python >=3.11; confirm the uv-managed environment for this pipeline targets 3.11+ before adding it as a dependency.

## Follow-ups
- Prototype an hour-of-day sweep logging pattern in Rerun (e.g. one timeline per sweep vs. a component on the same `run` timeline) to confirm the Viewer's scrubber UX before committing to a schema.
- Verify GPU-side EXR/HDR output from the OpenGL 4.3 engine round-trips correctly through tev's HDR display pipeline on the target workstation (12 GB NVIDIA GPU) once a first capture is available.
- Decide on a `.rrd` retention/rotation policy (per-run artifact vs. periodic `rrd optimize` pass) once real capture sizes from the lighting-fit pipeline are known.
