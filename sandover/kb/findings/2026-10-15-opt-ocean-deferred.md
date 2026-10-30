# Deferred ocean shading once per pixel (2026-10-15)

The deep-opt plan item "deferred ocean shading once per pixel", aimed at the water row of the
2026-10-10 profile (ocean-mid 5.5 + ocean-near 3.9 ms at village1-waterfront-1200, ocean-mid 6.0
at open-sea-low). Shipped as perf-opt bit 26 (`PERF_OPT_OCEAN_ONCE`, default on). Exact: no pixel
changes. Measured at 3840x2160, MSAA 2 + SMAA (the shipped default, both arms), every Sandover row
at its top tier, HDR PQ, perf-max.yaml spots, `wait.ok` true at every spot in every run.

## Symptom

The mid ocean's textured pass (`water_shade`, the most expensive fragment shader in the water)
draws after the opaque background with depth GEQUAL and depth writes, in the game's order. The
game sends the mid ocean back to front and draws some sections twice (a low-poly mesh, then a
high-poly one over it), so early depth rejection never fires and every hidden layer is shaded.

## How much overshading there is (probe)

Fragment shader runs of the mid and near textured draws, per frame, from a pipeline statistics
query (new counters `ocean-mid-mfrags`, `ocean-near-mfrags`, see perf.md). With bit 26 on, the mid
count is the pixels the mid covers, so off / on is the mid's depth overdraw. Run
`perf/ocean-deferred-probe1` (SSAO GPU timing switch on, so not a timing run).

| spot | mid off (M) | mid on (M) | overdraw | near (M) |
|---|---|---|---|---|
| village1-waterfront-1200 | 5.23 | 5.09 | 1.03 | 3.90 |
| open-sea-low | 4.99 | 4.00 | 1.25 | 1.75 |
| open-sea-high | 1.51 | 1.51 | 1.00 | - |
| beach-shore | 0.09 | 0.04 | 2.2 (tiny) | 3.60 |
| beach-whole-1730 | 1.04 | 0.51 | 2.0 | 2.00 |
| beach-whole-1820 | 0.98 | 0.51 | 1.9 | 2.15 |
| jungle-river-0800 | 0.11 | 0.11 | 1.0 | - |

The waterfront, the largest water cost, has almost no overdraw: its mid ocean covers 5.1 M pixels
(61 % of the 8.3 M at 4K) once each, so its 4.7-5.5 ms is one `water_shade` per covered pixel. The
tessellated mid there is 167k triangles (`WATER-PRIMS`), about 30 pixels each, so quad
overshading is small too. The near ocean is drawn blended without depth writes over a few big
triangles (12.5k); its layers composite in order and cannot be shaded once without changing the
look. What remains at the waterfront is the shader's own cost, not overshading.

## Mechanism of the fix

With bit 26 on, the mid ocean's tessellated deferred path draws its textured patches twice: a
depth-only pass (same program and polygon offset, `water_on` 0 and an unused `bucket` so the
fragment shader does one texture read, colour writes masked), then the textured pass at depth
EQUAL with depth writes off, then the envmap pass back at GEQUAL with writes on, as before.

Why it is exact: under GEQUAL with writes, a pixel (per sample) ends with the colour of the last
fragment whose depth equals the final depth. The depth pass computes that final depth; the EQUAL
pass then runs exactly the fragments at that depth, in the same order, so the last of them still
writes last. The two passes produce identical depths: same program, same inputs, `invariant
gl_Position` in the tessellation evaluation shader, same polygon offset. Derivatives and centroid
sampling depend on the triangle, not on the depth test, so each surviving fragment shades exactly
as before. Stencil writes (the SSAO exclusion bit) land on the same samples with the same value.
The depth pass's colour target is masked and the EQUAL pass rewrites every sample it touched.

Cost: the tessellation runs twice; the depth pass is 0.04-0.50 ms (`ocean-mid-depth`). It pays
where the overdraw is above about 1.2.

## Proof

Look: paused-frame A/B on the branch bin, masks 0xbfffffff / 0xbbffffff (bit 30 off as in the
harness run mask), wave clock pinned (`shots/ocean-deferred-ab26`): max 0, 0 pixels differ at
six spots. open-sea-high: A/B max 109 on 458 px, floor (A vs A2) max 135 on 635 px, B vs A2 max
100, all in one 35x30 box at the bottom-right edge (the blue emissive prop recorded as paused-state
motion in the deep-opt water2 row); every pixel outside it is 0.

Timing, back to back under one lane hold: base (play bin for master fb1b823c0) / branch / base,
each from a root staged by the same recipe, same `SANDOVER_DATA`. Frame ms, then the ocean
groups' GPU ms in the same order; last column the branch's own same-session marginal of bit 26
(off minus on, positive = the bit saves).

| spot | frame | ocean-near | ocean-mid (depth pass) | bit 26 saves (same session) |
|---|---|---|---|---|
| village1-waterfront-1200 | 16.33 / 16.36 / 16.33 | 3.20 / 3.18 / 3.19 | 4.66 / 4.71 / 4.67 (0.17) | -0.02 (noise 0.07) |
| open-sea-low | 26.34 / 25.69 / 26.38 | 1.62 / 1.62 / 1.63 | 5.69 / 4.64 / 5.43 (0.50) | 0.62 |
| open-sea-high | 19.71 / 19.77 / 19.76 | - | 0.98 / 1.02 / 0.98 (0.04) | -0.05 |
| beach-shore | 29.31 / 29.31 / 29.31 | 3.01 / 2.98 / 2.99 | 0.21 / 0.21 / 0.21 (0.05) | 0.08 |
| beach-whole-1730 | 26.75 / 26.80 / 27.40 | 2.61 / 2.62 / 2.61 | 1.77 / 1.13 / 1.77 (0.11) | 0.76 |
| beach-whole-1820 | 20.02 / 19.59 / 19.99 | 2.05 / 2.04 / 2.05 | 1.24 / 0.84 / 1.25 (0.11) | 0.54 |
| jungle-river-0800 | 23.21 / 23.20 / 23.11 | - | 0.19 / 0.25 / 0.19 (0.07) | -0.10 |

Outside the base drift: open-sea-low -0.65 ms (drift 0.04) and beach-whole-1820 -0.42 (drift
0.03). beach-whole-1730's frame sits inside its drift (0.65 between the base runs; the branch run
also had `shadow-merc` +0.21 from the actors) while its ocean-mid fell 0.64 ms and the same-session
marginal is 0.76. The waterfront, open-sea-high and jungle-river are neutral to 0.1 ms slower (the
depth pass with nothing to cull).

## Deferred

- The waterfront's water row stays: it is one shade per pixel. Taking a share of it needs a
  cheaper `water_shade` there or fewer shaded pixels, both visual judgement calls.
- A full deferred scheme (visibility buffer, then one full-screen shade per pixel) would also
  remove quad overshading, which matters only where the tessellated triangles are small
  (open-sea-low, about 10 pixels each), and changes the shading derivatives: a look change, not
  pursued.
- The depth pass could be skipped where it cannot pay (overdraw near 1); the counters are read a
  few frames late and only under the timing switch, so a runtime gate needs its own cheap signal.
  At most 0.1 ms is at stake.
