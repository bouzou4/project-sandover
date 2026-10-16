---
title: Texture dataset browser and visual regression report — buy vs build
date: 2026-09-23
issue: "#15"
workstream: tooling evaluation
status: verified
sha: 76e8eec
---

## Answer (<=5 lines)
FiftyOne (Apache-2.0) is a valid keep for the texture dataset browser: open-source core runs fully local with a bundled embedded MongoDB, supports arbitrary custom fields/tags edited in-App and read back in Python, and telemetry is a single config flag to disable. For visual regression, reg-cli is a replace: its CI matrix explicitly excludes Windows and its own docs tell Windows users to run under WSL/git-bash, and it stays on `-rc` releases. Pick a hand-rolled Jinja/static HTML before/after slider driven directly by the project's own NVIDIA FLIP output instead — zero new runtime, no Node toolchain, Windows-safe by construction, and it slots naturally next to the FLIP gate and Rerun logging already planned.

## Picks
| item | license (SPDX) | Windows | last release / last commit | maintenance | verdict | gotcha |
|---|---|---|---|---|---|---|
| FiftyOne (voxel51/fiftyone) | Apache-2.0 | Supported; `fiftyone-db` wheel bundles MongoDB binaries per-OS, no separate DB download at runtime | v1.22.0 (2026-09-12) / commit pushed 2026-09-22 | Active — 11.1k stars, 706 open issues, near-daily commits | keep | Telemetry (UUID import/App usage events) is opt-out by default; must set `FIFTYONE_DO_NOT_TRACK=true` or `fo.config.do_not_track=True` for the offline/private-assets requirement. Full app+embedded-DB stack is heavier than strictly needed for a few thousand small PNGs, but runs single-machine with no cloud dependency. |
| reg-cli (reg-viz/reg-cli) | MIT | Not officially supported — "Windows is not in the matrix" for CI (Node WASI path-translation issues); docs direct Windows users to WSL or git-bash | 0.19.0-rc2 (2026-09-05) / commit 2026-09-20 | Repo active (commits weekly) but still shipping release-candidates; is the renamed/rewritten successor of reg-suit (whose last stable, 0.14.5, is a year old) and inherited old unresolved issues (some open since 2017) | replace with hand-rolled Jinja/static HTML slider report | Adds Node.js v20+ as a second package/toolchain manager alongside the project's uv-managed Python; Windows path handling is explicitly unverified upstream. |
| odiff | MIT | Native binary, works on Windows | v4.5.0 (2026-07-23) | Active — 3.2k stars, commits within a month | not picked (reference only) | Fast native pixel diff, but produces a diff image/exit code only — no HTML report or slider UI; would still need a report layer built on top. |
| pixelmatch | ISC | Pure JS, works on Windows via Node | v7.2.0 (2026-04-29) | Active | not picked (reference only) | Diffing library, not a report generator — same gap as odiff, plus still needs Node. |
| BackstopJS | MIT | Works on Windows but heavy (Puppeteer/Chrome) | Last GitHub release tag v3.8.8 (2019-01-21) despite commits into 2026-09 | Stale releases — 577 open issues, no tagged release in ~7 years even though the repo is not archived | not picked (reference only) | Built for web-page screenshot regression (URL-driven scenarios), not a folder of renderer output images; release process effectively dormant. |
| Rerun (as diff viewer) | Apache-2.0 | Supported | pushed 2026-09-22, 11.5k stars | Active | not picked as the report tool | Already planned for pipeline logging and can show before/after image pairs + FLIP heatmaps as logged views, but has no built-in "before/after slider + pass/fail list" HTML report artifact — would require custom view/panel code, i.e. still a build, not a buy. |

## Evidence
- FiftyOne license Apache-2.0, 11,110 stars, 706 open issues, pushed 2026-09-22: `gh api repos/voxel51/fiftyone` (GitHub API, checked 2026-09-23).
- FiftyOne latest release v1.22.0 published 2026-09-12: `gh api repos/voxel51/fiftyone/releases/latest` (GitHub API, checked 2026-09-23).
- Open-source FiftyOne is Apache-2.0 and free for commercial use; FiftyOne Enterprise/Teams is the separate commercial product adding SSO, roles/permissions, service accounts, and the `fiftyone.management` API against a central server: https://docs.voxel51.com/enterprise/overview.html and https://voxel51.com/enterprise
- Bundled MongoDB is provided via the `fiftyone-db` package, which installs a MongoDB distribution for the local OS/hardware; a custom `database_uri` can override it: https://docs.voxel51.com/user_guide/config.html and https://docs.voxel51.com/installation/troubleshooting.html
- Telemetry: `do_not_track` config field / `FIFTYONE_DO_NOT_TRACK` env var controls "whether UUID based import and App usage events are tracked," default `False` (i.e., tracking on by default): https://docs.voxel51.com/user_guide/config.html
- Custom fields: arbitrary fields can be dynamically added to samples and are added to the dataset schema automatically; `Dataset.tags` stores arbitrary string tags; `Sample.metadata` holds type-specific metadata: https://docs.voxel51.com/user_guide/using_datasets.html
- In-App annotation supports ad hoc sample-by-sample metadata/label editing that "automatically save[s] to the database," readable back from Python: https://docs.voxel51.com/user_guide/annotation.html
- FiftyOne App launches in the default browser (or inline in Jupyter/Colab/Databricks/SageMaker notebooks) on local port 5151: https://docs.voxel51.com/user_guide/app.html and https://docs.voxel51.com/installation/environments.html
- reg-cli license MIT, 419 stars, 26 open issues, pushed 2026-09-23: `gh api repos/reg-viz/reg-cli` (GitHub API, checked 2026-09-23).
- reg-cli latest release 0.19.0-rc2 published 2026-09-05: `gh api repos/reg-viz/reg-cli/releases/latest` (GitHub API, checked 2026-09-23).
- reg-cli latest commit 2026-09-20 (sha b7a2b1e...): `gh api repos/reg-viz/reg-cli/commits` (GitHub API, checked 2026-09-23).
- reg-cli open issues include several open since 2017-2020, indicating carried-over backlog from the reg-suit lineage: `gh api "repos/reg-viz/reg-cli/issues?state=open&sort=created&direction=asc"` (GitHub API, checked 2026-09-23).
- reg-cli is drop-in compatible with classic reg-cli/reg-suit flags, `reg.json` schema, JUnit XML, and the `compare()` EventEmitter API; requires Node.js v20+; installed via `npm i -D reg-cli`; produces an HTML report via `-R` (or regenerates via `-F` from an existing `reg.json`); Windows is explicitly excluded from the CI test matrix due to Node WASI path-translation issues, with Windows users directed to WSL or git-bash: https://github.com/reg-viz/reg-cli
- reg-suit's own last stable release (0.14.5) predates the reg-cli rewrite by about a year, per npm/GitHub release history: https://www.npmjs.com/package/reg-suit and https://github.com/reg-viz/reg-cli/releases
- odiff license MIT, latest release v4.5.0 published 2026-07-23, 3,205 stars: `gh api repos/dmtrKovalenko/odiff` and `gh api repos/dmtrKovalenko/odiff/releases/latest` (GitHub API, checked 2026-09-23).
- pixelmatch license ISC, latest release v7.2.0 published 2026-04-29, 6,958 stars: `gh api repos/mapbox/pixelmatch` and `gh api repos/mapbox/pixelmatch/releases/latest` (GitHub API, checked 2026-09-23).
- BackstopJS license MIT, 7,181 stars, 577 open issues, repo pushed 2026-09-08 but latest tagged release v3.8.8 published 2019-01-21: `gh api repos/garris/BackstopJS` and `gh api repos/garris/BackstopJS/releases/latest` (GitHub API, checked 2026-09-23).
- Rerun license Apache-2.0, 11,484 stars, pushed 2026-09-22: `gh api repos/rerun-io/rerun` (GitHub API, checked 2026-09-23).

## Implications for the design
- Keep FiftyOne for the texture dataset browser as planned. Add an explicit setup step to set `FIFTYONE_DO_NOT_TRACK=true` (or `fo.config.do_not_track = True`) before first use, since the offline/private-assets requirement means default opt-out tracking cannot be relied on to stay off by policy alone.
- Drop reg-cli from the plan. Do not add a Node.js toolchain to a uv-managed Python project just for the visual-regression report, especially given reg-cli's own maintainers do not test or support Windows.
- Build the visual-regression report as a small Python module (Jinja2 templates + static HTML/CSS/JS, no bundler) that consumes the FLIP numeric output already planned as the pass/fail gate, renders a before/after slider per test case, and lists pass/fail using the same FLIP threshold. This keeps the toolchain to Python/uv only and produces a self-contained offline HTML file with no third-party report-generator dependency.
- Reserve Rerun for pipeline/frame-level logging as already planned; do not also make it the visual-regression report tool, since it lacks a ready-made pass/fail HTML artifact and would require custom viewer code equivalent to building the hand-rolled report anyway.
- odiff/pixelmatch remain useful only as reference implementations for diff-pixel math if the hand-rolled report needs a lightweight Python-side pixel diff; the project should implement diffing in Python (e.g., via existing image libraries) rather than pull in either JS library, to avoid the Node dependency entirely.

## Follow-ups
- Prototype the Jinja/static-HTML report against a small batch of real before/after renders and FLIP scores to confirm the slider UX and pass/fail table are sufficient before wiring it into CI/local workflows.
- Decide whether FiftyOne's dataset for texture review should be regenerated per game/project or persisted long-term, given the embedded-MongoDB storage footprint at thousands of samples.
- Re-check reg-cli's release channel in a future review; if it exits release-candidate status and gains an official Windows CI target, it could be reconsidered, but no action needed now.
