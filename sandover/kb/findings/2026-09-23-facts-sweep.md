---
title: Mood facts ledger sweep — verify the original specification's fact ledger against checked-out source
date: 2026-09-23
status: verified
sha: 76e8eec (76e8eeca35064ed83c3481b4eff51ee38c8ee361)
---

## Answer (≤5 lines)

Every non-external claim in the original specification's fact ledger was independently re-checked against source at
`76e8eec` (46 checkable items across renderer/mood/rig/textures, plus a full 21-level
mood slot-role table). Result: 39 verified, 2 refuted, 5 inferred-untestable-statically
(the priority `[I]` items — all settled as far as source alone can settle them, with the
runtime experiment spelled out for what remains). One initial "refutation" was itself
wrong and has been corrected in `facts/mood.md` — see below, it matters for unbake.

## Evidence (path:line@sha, short excerpts)

Curated ledgers, one per area, each a markdown table (Claim | Status | Source | Excerpt |
Note):
- `sandover/kb/facts/renderer.md` — 14 claims, 12 verified, 2 inferred-untestable
  (priority items 4 and 12; item 13 verified with a corrected mechanism).
- `sandover/kb/facts/mood.md` — 7 claim-table rows + 1 priority item + Village 1 sun
  numbers, all verified (after the kMaxColor correction below) except the priority item,
  which is inferred-untestable-statically as expected; plus the full per-level
  effect-slot role table for all 21 Jak 1 `update-mood-*` targets (20 named levels +
  `update-mood-default`) — this is the deliverable unbake needs.
- `sandover/kb/facts/rig.md` — 13 claim-table rows, 12 verified, 1 refuted (partial).
- `sandover/kb/facts/textures.md` — 4 claim-table rows, 2 verified, 1 refuted, 1 split
  (verified in-tree / inferred for the ecosystem-wide part).

### Refuted claims (both with full correction)

**1. `debug-set-camera-pos-rot!` does not read `*math-camera*`.**
`goal_src/jak1/engine/camera/cam-debug.gc:987` correctly switches to `cam-fixed`
(`(send-event *camera* 'change-state cam-fixed 0)`), but it writes its own `arg0`/`arg1`
parameters directly into `*camera-combiner*`'s `trans`/`inv-camera-rot` fields — it never
touches `*math-camera*`. Every call site passes a hardcoded bookmark matrix. `*math-camera*`
does have `trans`/`inv-camera-rot` fields (used elsewhere, e.g. `external-cam-reset!` right
below at line 1003), just not by this function. Practical effect for the rig: to place the
debug camera at a captured continue-point/bookmark pose, pass the pose directly as
`arg0`/`arg1` — don't expect to prime it by writing into `*math-camera*` first.

**2. No project doc warns against changing animated textures / sky-cloud-eye resolution / losing alpha.**
Searched `docs/` and the whole tree; no such modding/texture-replacement guidance doc
exists in this checkout (`docs/` only holds engine reverse-engineering notes). The exact
wording traces back to the original specification itself (the project's own scaffolding doc), not
an upstream or in-repo source. The underlying engineering rationale (alpha as an
envmap-fade channel) is plausible from engine notes but was never a stated warning.
Recommendation for the texture census and docs: if this guidance is wanted, it needs to be written, not cited
as pre-existing.

### A correction to an earlier (wrong) refutation — matters for unbake

`facts/mood.md` claim 7 (`kMaxColor`) was first reported refuted ("kMaxColor is 8, not 1")
by a worker that read the live working tree with `Grep`/`Read`. Checked directly against
the pinned commit with `git show 76e8eec:decompiler/level_extractor/fr3_to_gltf.cpp`:
`kMaxColor` is genuinely **1** at `76e8eec`, matching the original specification exactly. What's actually
going on: the working tree currently carries an **uncommitted** 42-line change (visible via
`git status`/`git diff`, not authored by this sweep) that bumps `kMaxColor` to 8 and adds a
`_COLOR_INDEX` glTF attribute — i.e. a concurrent process is already applying the exact
patch the unbake specification prescribes as its first step. This has been corrected in
`facts/mood.md` with a clear note. **Note for the unbake work:** the `kMaxColor`
patch + `_COLOR_INDEX` accessor may already be done/in-flight — check before redoing it.

## Implications for the design

- **unbake:** the mood system is far more heterogeneous than the original specification's
  five-primitive model. Surprises with real design impact, all in
  `facts/mood.md`'s Part 3 table: `jungleb`, `maincave`, `robocave`, `lavatube`,
  `citadel` (and effectively `darkcave`) bypass the day/night blend entirely via
  `update-mood-quick` — most/all of their palette slots aren't sun-driven at all.
  `darkcave` has zero sun slots (all 8 driven by a proximity-torch system,
  `*palette-fade-controls*`, an unlisted 6th "primitive" that also drives slots on
  `sunken`, `maincave`, `lavatube`, `finalboss`, `citadel`). `village3` overwrites the
  *source* `mood-lights-table` per frame rather than hijacking a slot weight. `sunken`
  discards its own `update-mood-palette` output and hardcodes `light-group 0` from a
  camera-depth lerp. `ogre`/`finalboss` cross-fade between multiple independent
  mood-context globals rather than hijacking slots in one context. Unbake's per-level fit
  plan should branch on these categories rather than assume uniform slot semantics.
- **SSAO (SSAO):** the existing bucket-30 injection point (`OpenGLRenderer.cpp:1346`) is
  the debug collision-mesh draw, not SSAO-ready — useful as a pattern only. The
  bucket-31 "opaque/transparent boundary" (`ALPHA_TEX_LEVEL0`) is real for level0 only;
  later buckets (level1 alpha, `MERC_AFTER_ALPHA`, water) mean it isn't one clean split
  across the whole frame.
- **stereo:** source alone confirms `perspective_matrix` is uploaded as an
  independent uniform, decoupled from bone (`ub_bones`) and light uniforms in
  `Merc2.cpp`/`merc2.vert` — the "perspective-only swap" is plausible and partly settled.
  What source cannot settle: whether GOAL-side bone-to-camera-space composition already
  assumes a single mono camera in a way that's per-eye-safe. Needs either a read of the
  GOAL skeleton/matrix code or the stereo runtime spike itself.
- **lightbench:** all static plumbing confirmed buildable (`Loader` own-thread,
  `TFragment::render_tree`/`Tie3::draw_matching_draws_for_all_trees` public and
  DMA-free, `runtime` is a static lib, `gfx_test.cpp` already opens a hidden window).
  Whether textures render correctly without game-provided animated-slot data needs the
  actual lightbench run.

## Follow-ups

- Anyone working on unbake: confirm whether the in-flight `kMaxColor`/`_COLOR_INDEX` change is
  theirs before duplicating it; once committed, re-cite `facts/mood.md` claim 7 against
  the new sha.
- `facts/*.md` files are currently **untracked** (not committed) per this task's
  instructions, since another process is committing to master concurrently — hand off to
  whoever integrates the KB.
