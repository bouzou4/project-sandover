# ADR-0006: Visual regression report
Status: accepted    Date: 2026-09-23    Deciders: project lead

## Context
The parity/regression workflow needs a human-readable before/after report driven by the
FLIP gate (see ADR-0008). The tooling evaluation's original plan was reg-cli. reg-cli explicitly excludes
Windows from its own CI matrix (its docs redirect Windows users to WSL/git-bash), is still
shipping `-rc` releases, and would add a Node.js toolchain next to this project's
uv-managed Python.

## Options (with cost/risk)
**reg-cli.** Cost: a second package manager/toolchain (Node v20+) purely for a report
generator. Risk: unsupported on native Windows by the maintainers' own admission; backlog
includes issues open since 2017.
**odiff / pixelmatch.** Fast native/JS pixel diff, but neither produces an HTML report or
slider UI — would still need a report layer built on top, in Node either way.
**BackstopJS.** Built for URL-driven web-page screenshot regression, not a folder of
renderer output images; last tagged release is from 2019 despite ongoing commits.
**Hand-rolled Jinja2 + static HTML slider, driven by FLIP output (chosen).** Cost: this
project owns and maintains a small report template. Risk: low — pure Python, no new
runtime, Windows-safe by construction, and slots directly next to the already-planned FLIP
gate and Rerun logging.

## Decision
Build the visual-regression report as a small Python module: Jinja2 templates + static
HTML/CSS/JS (no bundler), consuming the FLIP numeric output and error maps already
produced by the parity gate, rendering a before/after slider per shot plus a pass/fail
table using the same FLIP threshold. FiftyOne and Rerun remain the tools for interactive,
exploratory review (dataset browsing, pipeline/frame logging) — neither becomes the
pass/fail report artifact.

## Consequences
- No Node.js dependency anywhere in this project.
- The report template lives in `sandover/tools/` alongside the FLIP gate it consumes.
- Source: `kb/findings/2026-09-23-dataset-review.md`.
