---
title: The AO row's geometry cost is the stencil marks written before and during the opaque background
date: 2026-10-23
issue: ""
status: fixed (late stencil marks, perf-opt bit 5): mark and image parity verified, timing measured
sha: master ed0917a7b + debug probe (pc-set-ssao-option 4)
---
## Answer (≤5 lines)
At 4K on the current rig the AO row (AO + tile lights) costs 23.6 ms at beach-shore, 11.5 at
sunken and 24.8 at village1-hut-lamps, almost all inside `tie`, `tfrag` and `vista`. 21.1 / 9.0 /
22.1 ms of it is the per-bucket stencil marks (`SsaoRenderer::set_bucket_stencil`): any stencil
write in the sky buckets (0-4) or the opaque background buckets (5-30) slows every later
depth-tested background draw; later marks cost nothing. Fixed by writing those marks after
bucket 30 from depth copies: the row now costs 2.4 / 2.6 / 3.0 ms, images unchanged.

## Symptom
Profile 2026-10-22 (`2026-10-22-perf-rdna4.md`, the same rig): "SSAO row off" saved a median 9.4 ms against a
0.8 ms `ssao` pass. That run predates GTAO, when tile lights still required AO; on master AO off
with tile lights on keeps every prerequisite, so the row is measured here as `ssao+tile-lights`.

## Extra work per bucket with the row on (code read)
- Every bucket: `set_bucket_stencil` enables the stencil test with `GL_ALWAYS`/`GL_REPLACE`:
  buckets 0-4 write bit 7 (sky, far ocean: excluded), 5-30 clear bit 7 (background; bit 5 on merc
  level props with local lights), 31-44 clear bit 7, 45-47 set it (translucent actors), 48-56
  write 0x40 / mask 0xC0 (characters).
- Buckets 5-30 and 48-56: draw buffer 1 (RGB10_A2 normal, one sample under temporal AA) is
  enabled once per range (`set_bucket_normals`), not per draw; per draw `set_normal_write` sets
  `glColorMaski(1, ...)`; per tree two uniforms (`ssao_nrm`, `ssao_fogw`) and, with the sidecar,
  the vertex-normal buffer texture.
- Tile lights: draw buffer 4 (R16F sprite depth) with the emissive output; the emissive mask
  (buffer 2) also runs for bloom, so it is not part of the row.
- Once per frame: clears of buffers 1, 2, 4 before bucket 0; at bucket 56 the AO pass (depth read
  in place, linearize, mips, exclude mask, GTAO, history, denoise, composite).
- Not present: no second geometry pass, no per-bucket resolve or depth copy, no per-bucket FBO
  switch, no timer scopes inside the draw loops (one per bucket, as with the row off).

## Proof (same boot per table, 4K, config A of the 2026-10-22 profile, FSR 3.1 native)
Debug switch `(pc-set-ssao-option 4 MASK)` (gfx.h `ssao_prereq_probe`, no setting; bits in
`SsaoRenderer.cpp`). Marginal = frame ms saved against the base with the probe set. Run r10
(`$SANDOVER_DATA/perf/ao-prereq/r10`, noise bands 1.2 / 0.7 / 1.3 ms):

| measurement | beach-shore | sunken | village1-hut-lamps |
|---|---|---|---|
| base frame | 42.7 | 25.8 | 46.0 |
| AO + tile lights off (whole row) | 23.6 | 11.5 | 24.8 |
| 1: no stencil at all | 21.1 | 9.0 | 22.1 |
| 512: stencil test on, no writes anywhere | 21.1 | 9.3 | 21.9 |
| 1024: only sky buckets 0-4 write | 1.0 | 1.4 | 1.1 |
| 2048: only background buckets 5-30 write | 0.0 | 0.1 | 0.6 |
| 4096: only character buckets 48-56 write | 21.3 | 9.0 | 21.7 |
| 2: normal draw buffer never enabled | 1.4 | 1.0 | 1.0 |
| 32: normal target not attached | 1.0 | 0.9 | 1.7 |

Earlier runs, same setup (r1-r9): 4 (`ssao_nrm` 0) 0.1-0.7; 8 (face normals only) -0.6-0.1;
64 (no sprite depth) 0.2-0.4; 128 (no AO pass) 0.6-1.3; 256 (no per-draw normal colour mask)
-1.5-0.1; 16 (no emissive output) 5.9-7.1 (beach, hut-lamps), 1.2-2.2 (sunken): this one is the
bloom/tile-light emissive mask, which stays with the row off, so it is a separate cost.
Translucent-only writes (45-47) were free at beach-shore (r4).

Ruled out as levers on the stencil cost (each within noise of no change, stencil on): the depth
format (DEPTH32F_STENCIL8 0.1-0.6 slower; with stencil off it saves the same 9-22 ms), a
renderbuffer instead of the sampled texture, `glStencilMask(0xFF)` and scissor off at the frame
clear (the clear already runs with mask 0xff and scissor off, logged), `glInvalidateFramebuffer`
before the clear, `glClearBufferfi` instead of `glClear`, the linearize reading a depth copy
instead of the attachment (perf-opt bit 4 off), and detaching or skipping the exclude-mask FBO
that shares the depth-stencil texture.

## Mechanism
Measured, not explained at the hardware level: once the stencil plane of the render target holds
written values, the depth-tested draws that follow run about twice as slowly (tie 14.2 ms of the
village1-hut-lamps delta, vista 5.8). Writes after the background do not affect it in that
frame. No surface-level change tried (format, storage, clear, invalidate, sampling) avoids it, so
it is a property of this driver and GPU with stencil written ahead of heavy depth-tested
geometry, consistent with hierarchical depth culling being lost for the rest of the pass.

## Implications for the design
The marks written in buckets 0-30 are the cost; the ones written later are free. A fix keeps the
output by producing the sky / far-ocean / background part of the mask (bit 7, and bit 5 for merc
level props) without stencil writes before bucket 31, then writing it in one full-screen stencil
pass after bucket 30, before the translucent and character marks. Candidate sources: depth at
its clear value after bucket 30 for sky and far ocean (needs a check that buckets 0-4 write no
depth and that every bucket 5-30 draw that clears bit 7 also writes depth, else pixels under
non-depth-writing background draws change class), or a colour mark output in the background
shaders (the normal target's measured cost, about 1 ms, is the scale). Expected saving: 21.1 /
9.0 / 22.1 ms at these spots minus that pass (under 1 ms at 4K). Not done here: it touches every
background shader's output contract or relies on a depth invariant that is not yet verified, and
must be proven with a FLIP compare of the exclude mask, AO buffer and normal target.

## Fix: late stencil marks (`StencilMarks.h`, perf-opt bit 5)
Buckets 0-30 write no stencil (test off). After bucket 30, with vista and the sun disc drawn, one
full-screen pass per source writes the marks those buckets left, with the depth test `EQUAL`
against `gl_FragDepth` = a depth copy (per sample under MSAA) and the old write masks:

| source | copy | mark |
|---|---|---|
| sky, far ocean, vanilla mid ocean (0-4) | after bucket 4 | 0x80 / 0x80 where depth = copy, only if buckets 0-4 drew a sample (conditional render) |
| deferred mid ocean (after 18) | before (the water depth copy, 1 sample) and after | 0x80 / 0x80 where depth = after and after != before |
| merc level props (10, 17, local lights on) | before the first draw, after the bucket | 0x20 / 0xA0, same test |

Copies are `glCopyImageSubData` of the D24S8 texture, only for sources drawn this dispatch. The
other pixels keep the 0 the background buckets wrote. Buckets 31-56 write as before.

Preconditions (code read, then the comparator below):
- Buckets 0-4 do write depth in some paths: the mid ocean always (`depth ALWAYS` in bucket 4, or
  `GEQUAL` with writes when deferred after bucket 18), the far ocean whenever water shading is off.
  So "depth still at its clear value" is not enough on its own; the copy after bucket 4 and the
  copies around the ocean replace it.
- Every bucket 5-30 (and vista) draw that clears bit 7 also writes depth wherever it lands on a
  marked pixel: confirmed by the comparator (at most 3 old-only samples in a frame).
- Bit 5 cannot come from depth alone; the before/after copies around the prop buckets recover it
  exactly, so no early write is kept.

Comparator (`(pc-set-ssao-option 4 16384)`: per-bucket marks kept, late marks into scratch bits
0-1, `GL_SAMPLES_PASSED` per class over the whole screen). The first run (`check/`) drew its
queries with the last pass's `has_before` still set, so with an ocean or prop source they only
counted that source's region; fixed and rerun on the merged build with the conditional sky pass
(`check2/`, `check2-gk.log`): `marks-check.yaml`, 11 shots over 9 spots, 333 logged frames, FSR 3.1
(1 sample) and MSAA 4 (beach-shore, village1-hut-lamps), every source active somewhere. Maxima per
frame, in samples:

| shot | frames | bit 7 both (max) | bit 7 old only | bit 7 late only | bit 5 both (max) | bit 5 differ |
|---|---|---|---|---|---|---|
| beach-shore | 95 | 231875 | 0 | 0 (7037644 in its first frame, see below) | 21692 | 0 |
| sunken | 40 | 8294400 | 0 | 0 | 706136 | <= 1 |
| village1-hut-lamps | 12 | 1 | 0 | 0 | 705970 | 0 |
| open-sea-low | 41 | 7521900 | 0 | <= 1 | 8294400 | 0 |
| hut-roof | 26 | 5111756 | <= 3 | <= 2 | 14014 | 0 |
| jungle | 31 | 5110317 | <= 3 | <= 1 | 1187316 | <= 1 |
| village1-night | 19 | 8006726 | 0 | 0 | 72081 | 0 |
| maincave-crystals | 35 | 8294400 | 0 | 0 | 417067 | 0 |
| village2-night-lamps | 12 | 8294400 | <= 1 | 0 | 38561 | 0 |
| beach-shore MSAA 4 | 14 | 8294400 | 0 | 0 | 43066 | 0 |
| village1-hut-lamps MSAA 4 | 8 | 4548096 | 0 | 0 | 341924 | 0 |

Apart from one frame, at most 3 samples of 8.3-16.6 M differ either way. The exception is the
session's first compare frame, the frame SSAO and the probe were switched on together (the AO
targets are allocated in it): late marks on 7.04 M samples that the old marks left clear, against
about 0.23 M sky and ocean samples in every later frame at that spot. The other ten shot starts,
which also reload and reapply SSAO, show nothing. Cause not established; at worst one frame of
excluded AO when SSAO is switched on.

MSAA 4 paused A/B at beach-shore on the same build (`ab-msaa2/`): exclude and normals identical,
final 0.031 % (floor 0.039 %), AO 0.84 % (floor 1.36 %).

Paused-frame A/B (`--ab-mask 0xbfffffff 0xbfffffdf`, FSR 3.1, `$SANDOVER_DATA/perf/ao-late-marks/ab`):
final frame and AO (view 2) inside the A-vs-A floor at all three spots. The exclude/stencil view
(5) and normals view (3) differ in 0.06-0.12 % / 0.5-2.0 % of pixels, only along silhouettes (Jak
and geometry edges alike), with a zero floor: B is rendered 48 frames after A, A2 96, so B can sit
on another temporal-AA jitter phase; the late marks cannot change the normal target, which nothing
here writes. The controls below settle it: the differences are temporal AA state, not the marks.

## Parity and timing (2026-10-23, `$SANDOVER_DATA/perf/ao-late-marks/`)
One gk, `--parallel 1`, launched only with fewer than 2 gk running; one probe frame first
(`probe-msaa`). Staged as before: `gk.exe` from the worktree build in `$SANDOVER_DATA/bin/opt-ao-prereq`,
the worktree's `game/` in `$SANDOVER_DATA/worktrees/opt-ao-prereq-root`.

MSAA 4, no jitter (`ab-msaa.yaml`, `--ab-mask 0xbfffffff 0xbfffffdf`, `ab-msaa/`): the exclude/stencil
view and the normals view are identical at all three spots (A/B max 0, floor max 0); the final frame
and AO sit inside the A-vs-A floor:

| spot | final A/B (max, share) | final floor | AO A/B | AO floor |
|---|---|---|---|---|
| beach-shore | 23, 0.029 % | 25, 0.036 % | 49, 0.95 % | 76, 1.57 % |
| sunken | 32, 0.122 % | 53, 0.137 % | 46, 1.87 % | 57, 2.11 % |
| village1-hut-lamps | 52, 0.389 % | 49, 0.408 % | 42, 3.97 % | 54, 4.90 % |

FSR 3.1 control (`ab.yaml`, `--only sunken-exclude sunken-normals --ab-mask 0xbfffffff 0xbfffffff`,
`ab-aa/`): with the same mask on both sides, B (48 frames after A) differs from A in 0.062 % of the
exclude view and 0.574 % of the normals view, the same as the late-vs-per-bucket A/B at sunken
(0.060 % / 0.544 %), while A2 (96 frames) matches A. So the FSR-only silhouette differences are
temporal AA state between frames 48 apart, not the marks.

Timing, same boot, the r10 setup (`sandover-perf --bin opt-ao-prereq --only beach-shore sunken
village1-hut-lamps --frames 60 --toggles ssao+tile-lights --custom late-marks clear:5 mask:0xbfffffff
--custom p1 "(pc-set-ssao-option 4 1)" "(pc-set-ssao-option 4 0)"`, `t1/`, no other gk). Per-bucket
marks = base minus the `late-marks` marginal:

| measurement (ms) | beach-shore | sunken | village1-hut-lamps |
|---|---|---|---|
| frame, per-bucket marks (bit 5 off) | 41.6 | 24.6 | 44.4 |
| frame, late marks (base) | 21.2 | 17.6 | 23.8 |
| saving | 20.4 | 7.1 | 20.6 |
| `ssao-marks` group | 0.70 | 0.85 | 0.81 |
| AO + tile lights off (whole row) | 2.4 | 2.6 | 3.0 |
| p1: no stencil at all | 0.8 | 1.8 | 1.0 |
| noise band | 0.16 | 0.64 | 0.21 |

The row drops from 23.6 / 11.5 / 24.8 ms (r10) to 2.4 / 2.6 / 3.0. What no stencil still saves is
mostly the marks pass itself (`ssao-marks`); at sunken another 0.5 ms sits in `sky-blend`, the
buckets 31-44 clears.

## Follow-ups
- The marks pass (0.7-0.85 ms at 4K) is up to four full-screen passes plus depth copies; cheaper
  variants (one pass, stencil-only copies) are possible once optimizations are on the table.
- The emissive mask output costs 5.9-7.1 ms at the tie-heavy spots with bloom on; worth its own
  attribution (partial colour masks on buffer 2, RGBA8 with highlight material).
- Run data: `$SANDOVER_DATA/perf/ao-prereq/r1`..`r10`; probe bits as in `SsaoRenderer.cpp`
  (renumbered after r9: r10's 512/1024/2048/4096 = r2's 8192 and r4's 65536/131072/262144).
