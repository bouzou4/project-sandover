---
title: Background depth prepass (tfrag, tie, shrub): overdraw measured, prepass built, parity at the floors
date: 2026-10-23
issue: ""
status: done; 3.2 / 5.2 / 5.7 ms saved at beach-shore / jungle / village1-hut-lamps (4K, same boot), village1-night inside its noise; every A/B view at its same-mask control; vanilla path unchanged
sha: master 376d4cb19 merged into opt/background-prepass
---
## Answer (5 lines)
On current master the background shades 21-36 M fragments per frame for 8.3 M pixels (2.6-4.3
per pixel). A depth prepass of the opaque tfrag and tie of each level (and of each shrub bucket),
then the shaded draws with depth writes off, saves 3.18 / 0.13 (noise 0.88) / 5.24 / 5.68 ms of
frame at beach-shore / village1-night / jungle / village1-hut-lamps on the merged head. Paused A/B
equals the same-mask control in all 12 views (final, normals, emissive); vanilla frames match master.

## Setup
`sandover-perf`, 3840x2160, 60-frame windows, FSR 3.1 native, play defaults, one gk, no other gk
(gate on the process list and GPU 3D load before each boot). Staged builds `opt-prepass-1`
(counters + prepass, default off), `opt-prepass-2` (branch head, default on), `opt-prepass-ab`
(branch head plus an uncommitted proof hook, below). Root `$SANDOVER_DATA/worktrees/opt-prepass-root`.
Data: `$SANDOVER_DATA/experiments/opt-prepass/` (`m1` timing + counters, `ab17` paused A/B,
`ctl17` control, `bis-*` bisect, `probe` first frame).
Final pass: `opt-prepass-3` (master 376d4cb19 merged), `m3` timing, `abctl` control + A/B,
`van-master` / `van-branch` vanilla.

## What is built (commit on opt/background-prepass)
- `(pc-set-ssao-option 5 MASK)`, `BgPrepass` in gfx.h: 1 tfrag + tie per level (buckets 6-9,
  13-16), 2 each shrub bucket (20, 26), 4 shaded draws of the prepassed set without depth
  writes, 8 probe: no cut, 16 probe: both levels in one run. Default 7. Gated on the Sandover
  render path, no stereo, multidraw, inspect off, so the vanilla path never runs it.
- The dispatcher copies the DMA follower, replays the run's background buckets with
  `SharedRenderState::bg_prepass` set (colour masks locked off by `SsaoRenderer::color_lock`),
  then renders the buckets as before. Same programs and uniforms as the shaded pass, so depth is
  bit-identical; `bg_depth_only` returns at the top of tfrag3.frag / shrub.frag (1) or after the
  alpha test on the same texel (2). No stencil, no FBO switch.
- Prepass set: depth-writing, unblended, GEQUAL draws (tfrag all trees, tie NORMAL category; wind,
  envmap and their second draws stay out). A blended depth writer or a depth write under ALWAYS
  (wind draws included) cuts the rest of the run (counter `bg-prepass-cuts`), which keeps every
  pixel's colour: a blended writer drawn before an occluder could otherwise lose what it blends
  over.
- Counters (shadow probe 16): `bg-tfrag-mfrags`, `bg-tie-mfrags`, `bg-shrub-mfrags`,
  `bg-vista-mfrags`, `bg-pre-mfrags`; group `bg-prepass`.

## Fragments per pixel (probe 16, M per frame; 8.29 M pixels)

| spot | tfrag | tie | shrub | vista | shaded total (/px) | with prepass 3: shaded | prepass frags |
|---|---|---|---|---|---|---|---|
| beach-shore | 8.13 | 12.26 | 0.49 | 0.45 | 21.3 (2.6) | 12.2 | 20.6 |
| village1-night | 15.22 | 10.26 | 4.50 | 3.69 | 33.7 (4.1) | 22.4 | 27.0 |
| jungle | 11.09 | 22.55 | 2.00 | 0 | 35.7 (4.3) | 18.0 | 34.0 |
| village1-hut-lamps | 22.85 | 9.51 | 0.07 | 0 | 32.4 (3.9) | 11.9 | 32.5 |

## Final timing (run m3, merged head, same boot, 3840x2160, MSAA 2 run setting, FSR 3.1 native)

`sandover-perf --only beach-shore village1-night jungle village1-hut-lamps --custom bgp-off
"(pc-set-ssao-option 5 0)" "(pc-set-ssao-option 5 7)"`, one gk, no other gk, GL adapter checked.

| | beach-shore | village1-night | jungle | village1-hut-lamps |
|---|---|---|---|---|
| frame, prepass on (ms) | 27.41 | 21.44 | 23.80 | 21.20 |
| frame saved | 3.18 | 0.13 (noise 0.88) | 5.24 | 5.68 |
| tfrag + tie + shrub saved | 3.61 | 2.90 | 7.14 | 6.08 |
| `bg-prepass` group | 0.45 | 0.58 | 0.93 | 0.63 |
| `bg-prepass-cuts` | 0 | 0 | 0 | 0 |

At village1-night the background groups save 2.3 ms net, but the off window had most other groups
0.1-0.6 ms faster (motion blur, FSR 3, shadows), so the frame delta sits inside the 0.88 ms band.
Base frames are 3-12 ms higher than run m1's on the same adapter (PCI bus 3); the cause is not
established and the marginals are same-boot.

## Timing (run m1, same boot, frame ms saved against the prepass off; noise 0.24-0.42)

| mode | beach-shore | village1-night | jungle | village1-hut-lamps |
|---|---|---|---|---|
| base frame (off) | 18.9 | 17.9 | 19.9 | 18.1 |
| 1 tfrag + tie | 1.54 | 1.19 | 3.41 | 3.13 |
| 3 + shrub | 1.37 | 1.08 | 3.38 | 3.29 |
| 7 + no depth writes (default) | 1.95 | 2.46 | 4.71 | 3.81 |
| 2 shrub only | -0.02 | 0.09 | 0.10 | 0.12 |
| 19 cross-level probe | 1.46 | 1.31 | 3.40 | 3.17 |
| `bg-prepass` group (mode 7) | 0.28 | 0.41 | 0.64 | 0.41 |

Ceiling: the shaded tfrag + tie time drops from 3.6 / 4.4 / 7.0 / 5.8 ms to 1.5 / 1.8 / 2.3 /
1.7 ms (mode 7). Cross-level runs add nothing; shrubs about 0.1 ms. Depth writes off matter (the
shaders contain `discard`, so with writes on the driver tests late).

## Parity (paused A/B, MSAA 4, `abctl.yaml`, one boot)
Hook build `opt-prepass-ab` (the prepass also follows perf-opt bit `$SANDOVER_BG_PREPASS_AB_BIT`,
not committed). Each of the 12 views is shot twice back to back: the A/B (`--ab-mask 0xffffffff
0xfffdffff`, bit 17 off = prepass off in B) and the control (`-ctl`: B mask set back to
0xffffffff after the load, prepass on in both). FLIP mean of A against B; pixels over 2 in brackets.

| view | prepass A/B | control A/B | A/A2 |
|---|---|---|---|
| beach-shore final | 0.0066 (0.458 %) | 0.0066 (0.457 %) | 0.0034 |
| beach-shore normals | 0.0176 (1.30 %) | 0.0184 (1.36 %) | 0 |
| beach-shore emissive | 0.0021 (0.118 %) | 0.0025 (0.144 %) | 0.0010 |
| village1-night final | 0.0054 (0.419 %) | 0.0058 (0.514 %) | 0.0042 |
| village1-night normals | 0.0556 (9.92 %) | 0.0556 (9.93 %) | 0 |
| village1-night emissive | 0.0017 (0.068 %) | 0.0016 (0.065 %) | 0.0013 |
| village1-hut-lamps final | 0.0059 (0.308 %) | 0.0058 (0.299 %) | 0.0057 |
| village1-hut-lamps normals | 0.0211 (1.92 %) | 0.0217 (2.13 %) | 0 |
| village1-hut-lamps emissive | 0.0017 (0.021 %) | 0.0018 (0.027 %) | 0.0017 |
| jungle final | 0.0099 (1.38 %) | 0.0104 (1.43 %) | 0.0089 |
| jungle normals | 0.0495 (5.71 %) | 0.0489 (5.97 %) | 0 |
| jungle emissive | 0.0037 (0.055 %) | 0.0039 (0.064 %) | 0.0031 |

Every view sits at its control (largest excess 0.0008 FLIP, emissive at village1-night 0.0001):
the A/B difference is the 48-frame gap between A and B, not the prepass, so no bisect was run.
Cross-check: B of the A/B against B of the control differs as much as their two A frames (prepass
on in both) at every view, e.g. village1-hut-lamps final 78.52 % vs 78.52 %, jungle final 2.90 %
vs 2.78 %; the two shots differ by shot-to-shot state, the same with and without the prepass in B.

## Vanilla (auto-v0, 2560x1440 MSAA 4, all off, `--parallel 1`)
Master 376d4cb19 (staged `perf-rebaseline` and its root) against the merged branch, same capture
command, pixels differing at all (threshold 0):

- village1-hut and beach-start, all four hours: bit-identical.
- jungle-start 0.12-0.16 %, sunken-start 0.07-0.44 %: inside the rig's floor at those shots (two
  branch runs differ by 1.24-1.44 % at jungle and up to 0.18 % at sunken; master against master on
  2026-10-18 by 0.14-0.39 % and 0.04-0.26 %; animated actors).
- The gate is in code: the prepass needs `sandover_path`, which vanilla never sets.
- `sandover-parity check --captures` against the 2026-09-23 golden: master and branch both fail
  the same 8/16 shots with the same FLIP (equal to 4 decimals; village1-hut-noon 0.0194 in both,
  threshold 0.0114). Master has moved off that golden; its refresh is a separate job.

## Open
- village1-night's frame delta is inside its noise on the merged head; one more same-boot pair
  there would settle it (its background groups save 2.3 ms net).
- Shrub runs (bit 2) save 0.2-0.37 ms in the shrub group at three spots; kept.
- The proof hook stays uncommitted (`ab_hook.py` patch: an environment-selected perf-opt bit).
