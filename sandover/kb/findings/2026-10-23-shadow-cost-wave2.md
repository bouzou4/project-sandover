---
title: Shadow cost, second wave (attribution on the current rig, exact receiver savings)
date: 2026-10-23
issue: ""
status: two exact receiver changes and two scratch-array fixes land; caster cull and kernel loop rejected; overdraw is the plan item
sha: master d209dcf74 (attribution first on 4a078e70a)
---
## Answer (5 lines)
The shadows row is receiver cost, not map cost: the world map prepass is 0.56-0.81 ms for three
4096 cascades redrawn every frame, the actor maps 0.03-0.04 ms. The rest is the per-fragment world
map lookup in tfrag, tie, shrub and vista, run on 71-88 M background fragments per frame for 8.3 M
pixels (8.5-10.6x). On current master this pass saves 0.3 ms (beach-shore), 0.4 ms
(village1-night) and 1.8 ms (village1-hut-lamps) of a 2.0-4.6 ms row, every change exact.

## 1. Setup
- Same rig and method as `2026-10-22-perf-rdna4.md`: `sandover-perf`, 3840x2160, 60-frame
  windows, A/B/A per toggle inside one boot, one instance, no other gk (gate on the process list
  before each boot; backoff when another gk ran), play defaults (`sandover-settings-reset!`,
  shadow quality high, FSR 3.1 native). Spots: beach-shore (15 h, sun), village1-night (23 h,
  moon), village1-hut-lamps (22.5 h, moon plus two lamp maps).
- Builds: the branch staged as `opt-shadows2-p1` (master 4a078e70a plus probes) for the
  attribution, `-p2`/`-p3` for the wins, `-p4` (merged master d209dcf74) for the final numbers.
  Data: `$SANDOVER_DATA/experiments/opt-shadows2/` (`attr1`-`attr4`, `meas1`, `final`, `ab2-*`,
  `caps/`).
- Probes: shadow option 44 (`shadows.md`): 1 one compare tap, 2 no world lookup, 4 no actor sun
  map, 8 no lamp maps, 16 count background fragment shader runs (counter `bg-mfrags`).

## 2. Attribution (master 4a078e70a, run attr1, frame ms saved by switching off)

| | beach-shore | village1-night | village1-hut-lamps |
|---|---|---|---|
| frame | 43.6 | 44.9 | 45.6 |
| shadows row | 8.24 | 9.25 | 11.19 |
| world map off (32-34) | 6.92 | 8.47 | 10.06 |
| world map on the background only (32) | 5.76 | 7.34 | 9.04 |
| prepass `world-shadows` GPU (3 x 4096, cascade 0 in a 2048 corner, 579 draws, 0.56 M tris at the hut) | 0.81 | 0.56 | 0.75 |
| actor sun map + spot maps (`shadow-merc`, `-generic`, `-clear`) | 0.03 | 0.03 | 0.03 |
| actor sun map lookups (probe 4) | 0.10 | 0.29 | 0.22 |
| lamp map lookups (probe 8) | 0.00 | 0.06 | 0.54 |
| local lights on merc (option 7) | 0.00 | 0.00 | 0.68 |
| canopy / sky hue / landing disc | 0.63 / 0.31 / 0 | 0.51 / 0.17 / 0 | 0.45 / 0.37 / 0 |
| background fragment runs, M per frame (probe 16; 8.29 M pixels) | 70.7 | 77.7 | 88.0 |

Receiver splits: one compare tap in place of the search and kernel (probe 1) saved 1.25 / 0.77 /
2.34; no lookup at all (probe 2) 2.25 / 1.50 / 3.44. Both are lower bounds: where a lookup is
skipped the PBR sun terms run as if lit. Most of the remaining gap to option 32 off was the bit 31
gate's PBR clause: `pbr_sun_occ_read()` cost 3.4-5.7 ms, almost all of it the porosity const table in
scratch memory that the porosity-table change then removed on master.

After the late stencil marks and the porosity-table change (master d209dcf74, run ab2) the frame is 18-19 ms and the shadows row 2.0-4.6
ms at the same spots; the receiver cost is still the largest part of it.

## 3. Changes (all exact; numbers on master d209dcf74)

| change | beach-shore | village1-night | village1-hut-lamps | proof |
|---|---|---|---|---|
| bound grid early-outs (perf-opt bit 6) | 0.03-0.13 | 0.03-0.25 | 0.79-1.03 | paused A/B bit on/off at the floor |
| gate: no world lookup where `pbr_sv` is 0 (bit 31) | 0.00-0.23 | 0.04-0.12 | 0.00-0.10 | paused A/B gate on/off at the floor |
| cascade extents as a `vec3`, not `float e[3]` | 0.19-0.21 | 0.26-0.30 | 0.36 | cross-boot frame, FLIP within the cross-boot floor |
| local-light tints as selects, not `kLocalTint[4]` | 0 | 0 | 0.19 | same |
| same boot, both bit-gated changes off vs on (`before-all`) | 0.12-0.13 | 0.09-0.14 | 1.24-1.28 | |
| total | about 0.3 | about 0.4 | about 1.8 | |

On 4a078e70a (before the porosity-table change) the gate change saved 0.46 / 1.94 / 1.21 ms and the bound grid 0.30 /
0.25 / 1.91 (run meas1); most of the gate's gain was the porosity table that change fixed.

**Bound grid (bit 6).** After a cascade is drawn, two full-screen passes (`world_shadow_mm.frag`)
store per 8x8-texel cell a least-squares depth slope g, M = the least `d - g.x` over every texel a
lookup from that cell can read (search radius plus 2 texels, rounded up to whole cells: windows of
7x7, 5x5 and 3x3 cells for the 21, 13 and 5 texel reaches of cascades 0-2) and the largest depth.
A receiver returns 1 when `M + g.x0 - ref >= |dz - g| f + |g| texel` (its plane, bounded over the
footprint f, is at or in front of every texel: no search tap blocks and every compare tap passes)
and 0 when the largest depth is in front of `ref - |dz| f` (every compare tap fails). Both are the
values the full kernel returns. Water lookups (6x penumbra) and cells whose window crosses the
layer edge take the full kernel. The debug view `(pc-set-shadow-option 2 5000000)` paints tfrag
by outcome: at village1-night 20 % lit early-outs, 22 % blocked, 28 % full lookups (edges, the
hut walls whose slope differs from the cell's fit); at the hut 94 % blocked. Grid cost: inside
`world-shadows`, +0.13-0.18 ms with every cascade redrawn.

**Gate (bit 31).** The PBR glint and wet-film sun terms multiply by the PBR sun share `sv`; with a
sidecar record `pbr_sv` 0 means `sv` and `pbr_ksun` are 0, so the parallax term, the glint and the
film lobe ignore the occlusion. The gate now asks `pbr_sv != 0.0` before `pbr_sun_occ_read()`.

**Scratch arrays.** This driver keeps dynamically indexed local and const arrays in per-lane scratch
memory (the porosity table). `e[c]` in `ws_sun_vis_c` and `kLocalTint[i]` were two more; selects replace them. The
cross-boot base frames were stable to 0.03 ms over four boots (old 19.04/18.23/18.38, new
18.85/17.97/17.83 with both fixes, old again 19.06/18.27/18.39). `kWsDisk` and `kWsDisk8` are read
in constant-trip loops; removing the extents array was enough, they were not changed.

## 4. Look (FLIP at 4K)
Paused-frame A/B on the MSAA path (`--goal-after-load "(pc-set-aa 0 0)"`), each frame against its
own A/A2 floor (the paused frame is no longer bit-stable: tile lights, water, particles):

| A/B | beach-shore | village1-night | village1-hut-lamps |
|---|---|---|---|
| bound grid on vs off | 0.0015 (floor 0.0016) | 0.0035 (0.0038) | 0.0056 (0.0065) |
| gate on vs off (bit 31, new clause included) | 0.0015 (0.0017) | 0.0035 (0.0037) | 0.0059 (0.0062) |
| scratch-array fix, cross-boot | 0.0096 (cross-boot floor 0.003-0.010) | 0.0089 (0.009-0.013) | 0.110 (0.10-0.17) |

The rig's verdict is `floor` for every A/B. FLIP's share of pixels over its threshold matches the
floor too (0.002 % / 0.06 % / 0.1 %). The cross-boot hut floor is large because the hut-lamps
framing shifts between boots (same requested camera); not caused by these changes.

## 5. Tried and rejected
- Caster cull against the view frustum's footprint across the light (a 2D hull, casters padded 2
  m): 0.0-0.08 ms; the prepass is under 1 ms in all. Removed.
- One cascade-kernel call site in a loop with a data-dependent trip count (against unrolling):
  cross-boot identical (within 0.1 ms). Reverted.
- Not pursued, by the attribution: static-caster caching across frames (prepass under 1 ms, and
  perf-opt bit 30 already keeps still cascades), lamp maps at a lower rate (0.03 ms), resolution by
  coverage (prepass), kernel taps where the penumbra is sub-pixel (the minimum penumbra is 1.5
  texels, about 1.4 cm in cascade 0, 5-7 px at 5 m; the outer cascades already take 8 taps).

## 6. Plan item (not done here)
Background overdraw: 71-88 M fragment shader runs per frame for 8.3 M pixels. The world map
lookup, the PBR material path and the normal/emissive writes all run on hidden fragments. A depth
prepass (or early depth on draws without alpha test) is the "one lookup per pixel" lever, a
multi-renderer change (tfrag, tie, shrub, vista) beyond this item; size it with `bg-mfrags` and
`tie-mfrags`.
