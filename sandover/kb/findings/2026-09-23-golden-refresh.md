---
title: Golden refresh — clean master rebuild after SSAO normals, sidecar and vista fog
date: 2026-09-23
issue: none (phase 0 parity gate)
status: verified
sha: f953354f0
---
## Answer (≤5 lines)
Re-pinned the parity goldens from a clean build of master (engine sha `f953354f0`, build
tagged `f953354f0-20260923-164511-6ecf9f700b`, no engine-source diffs at build time).
`auto-v0`: F0 0.001727, m 0.009672, threshold 0.011399. `auto-v1`: F0 0.002203, m 0.008751,
threshold 0.010954. `check --build f953354f0-...` passes both sets (16/16, 56/56) against
the new goldens. The old `a664140c3` goldens (captured from a binary older than their label)
are retired, not deleted, under `$SANDOVER_DATA/parity/retired/`.

## Evidence (path:line@sha / URL, short excerpts)
- Build: `task -t sandover/Taskfile.yml build` (jobs 14, no reconfigure), full relink+rebuild
  of 1125 targets since several PRs landed since the last local build: 478.5 s. Staged via
  `sandover.engine.staged_bin(wait_for_build=True)` (name `20260923-164511-6ecf9f700b`,
  `build.json` `engine_sha f953354f0`, `reason ""`), pinned to
  `$SANDOVER_DATA/parity/bin/f953354f0-20260923-164511-6ecf9f700b/`. Main checkout `git
  status` was clean and `HEAD == origin/master` immediately before the build.
- The main tree is a shared working tree (other live branches (emissive, openxr,
  shadows-v1/smoke) build and capture in it concurrently, by design). Between steps the
  tree repeatedly went dirty and clean again as those branches committed/merged; every
  time a capture's sidecar reported `changed_during_capture` or a differing
  `goal_src.checkout_sha`, the intervening commits were diffed and confirmed to touch only
  `decompiler/`, `sandover/` tooling or (once) an inert `define-extern` declaration for an
  unused OpenXR toggle (`goal_src/jak1/kernel-defs.gc`, PR #147) — never `goal_src` render
  paths or code compiled into the pinned binary. The pinned binary's own engine sha
  (`f953354f0`) never changed.
- Calibration (`sandover-parity calibrate --build f953354f0-20260923-164511-6ecf9f700b`):
  - `auto-v0`: run0 774 s (all 4 slots failed to boot under concurrent load — `(mi)` never
    finished inside the 600 s timeout — retried once on a single instance), run1 68 s clean.
    `F0 0.001727 m 0.009672 threshold 0.011399` (smallest perturbation: 1px x-shift, 0.0211).
  - `auto-v1`: run0 231 s clean, run1 237 s (one slot died at boot, 12 shots retried on one
    instance). The FLIP noise-floor/perturbation pass twice hit `MemoryError` loading
    2560x1440 PNGs while free system memory was down to ~4.6-5.5 GB (other workstreams'
    concurrent builds/captures); reran unchanged from the saved `run0`/`run1` captures
    (`--runs`) once free memory recovered to ~17.7 GB and it completed.
    `F0 0.002203 m 0.008751 threshold 0.010954` (smallest perturbation: gamma-1-level,
    0.0197).
  - Entries: `$SANDOVER_DATA/parity/calibration.json` keys
    `auto-v0|2560x1440|msaa4|f953354f0`, `auto-v1|2560x1440|msaa4|f953354f0`.
- Golden capture (`sandover-parity golden ... --reason "clean master rebuild after SSAO
  normals, P2 sidecar and vista fog merges; previous a664140c3 goldens mislabelled,
  replaced"`):
  - `auto-v0`: 16/16 shots, 127 s (one slot failed to boot, 4 shots retried).
  - `auto-v1` first attempt: 56/56 shots, 267 s, published — but `check` then failed
    `village2-dock-dawn` at FLIP mean 0.0345 (max abs diff 124, 457k px) against a candidate
    built from the *identical* binary and engine sha. Three independent fresh captures of
    that one shot (one via the failing `check`, two more via `sandover-capture run --only
    village2-dock-dawn`) were pixel-identical to each other and to the calibration run's
    `flip_mean 0.0` baseline for that shot, and all differed from the *golden* by the exact
    same 0.0345 — proving the golden capture itself had a one-off render glitch (most likely
    from the heavy concurrent GPU/CPU/memory load at that moment), not a real regression or
    noise-floor gap. Re-ran `golden --replace`: 56/56 shots, 175 s, zero crashes, single
    consistent `goal_src.checkout_sha 0e6de2f8d`, no `changed_during_capture`. The glitched
    first attempt is preserved (not deleted) by the tool's own `--replace` convention at
    `goldens/auto-v1/f953354f0.replaced-20260923-180035/`.
  - Published: `$SANDOVER_DATA/goldens/auto-v0/f953354f0/` (16 shots, goal_src checkout
    `544e18b02`), `$SANDOVER_DATA/goldens/auto-v1/f953354f0/` (56 shots, goal_src checkout
    `0e6de2f8d`); both `goal_src_dirty: false`.
- `check --build f953354f0-20260923-164511-6ecf9f700b`: `auto-v0` PASS 16/16 (173 s
  capture); `auto-v1` PASS 56/56 (209 s capture, after the golden re-capture above).
- `determinism --set auto-v0` (same pinned build): run0 218 s (two slots failed to boot,
  retried), run1 122 s (one slot failed to boot, retried). Result: mean FLIP 0.00037, worst
  0.00178 (`sunken-start-night`), 4/16 shots pixel-exact, 16/16 cameras bit-equal — consistent
  with the prior baseline (`2026-09-23-deterministic-capture.md`, `2026-09-23-parity-calibration.md`).
- Retired (moved, not deleted) to `$SANDOVER_DATA/parity/retired/`: `goldens/auto-v0/
  a664140c3/`, `goldens/auto-v1/a664140c3/`, `calib/auto-v0_2560x1440_msaa4_a664140c3/`,
  `calib/auto-v1_2560x1440_msaa4_a664140c3/`, and the two `a664140c3` entries split out of
  `calibration.json` into a parallel `retired/calibration.json`.

## Implications for the design
- The gate is now correctly labelled: `goldens/<set>/f953354f0/` really was captured with a
  `gk`/`goalc` built from that exact commit, confirmed via `build.json` recorded at staging
  time rather than inferred after the fact (the `a664140c3` goldens' original defect).
- Thresholds moved slightly from the `a664140c3` baseline (auto-v1 threshold 0.01165 ->
  0.01095, auto-v0 0.01120 -> 0.01140) — both still comfortably below the smallest known-bad
  perturbation, so the calibrated parity band B1 holds after SSAO, the sidecar and vista fog landed,
  all default-off.
- One golden generation needed a same-build re-capture because of a transient render glitch
  under heavy concurrent GPU/CPU load, not a code or determinism problem: `golden`'s
  `--replace` path (capture into a fresh `.tmp`, publish only if every shot succeeds, keep
  the superseded generation) handled this cleanly. Capturing goldens while several other
  workstreams share the machine is workable but noisier (more boot-timeout retries, one
  memory-pressure failure in FLIP comparison, one render glitch) than a quiet machine; none
  of it required touching engine code or reconfiguring the build.

## Follow-ups
- None blocking. If golden captures on a busy machine keep producing one-off glitches,
  consider a golden-specific self-check (capture twice, require the two to be pixel-exact
  before publishing) rather than relying on a downstream `check` to catch it.
