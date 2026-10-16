# World map: cheaper soft shadow in the outer cascades, first cascade at 2048 (2026-10-15)

Second-wave item on `2026-10-10-perf-profile.md` (b)/(c): "a smaller kernel in the outer cascades"
and "a 2048 first cascade with 4096 outer ones". Both sit behind perf-opt bit 28
(`PERF_OPT_WORLD_KERNEL` = `PERF_OPT_CORE + 0`, uniform `ws_kernel`, default on). Unlike the first
wave this is a look change, within the maintainer's bound ("I probably won't notice the fine margins").
Cascade 0 keeps master's full kernel; day and night take the same path (the moon is the same map
and the same filter).

## Mechanism

**Item A, outer cascades (1: 10-35 m, 2: 35-120 m).** Master's per-cascade lookup is a 9-tap
blocker search (single depth reads), then 4 compare taps when nothing blocks or 16 Poisson
compare taps over the penumbra the blockers size. With the bit on, cascades 1 and 2 search with
5 `textureGather` reads of the raw depths (the centre 2x2 and four 2x2 footprints on one ring at
1.04 x the search radius, 20 texels), return lit when none blocks, and otherwise take 8 compare
taps over the blocker-sized penumbra. After review:

- The 8 taps are a zero-mean set (`kWsDisk8`, four antipodal pairs). The first version took the odd
  half of master's 16-point disk, whose centroid sits 0.26 kernel radii off along +u, so every
  outer penumbra was shifted sideways and the cascade 0/1 blend band could double an edge.
- Each gathered texel is compared with the receiver plane at its own centre (offsets from
  `fract(uv * size - 0.5)`). With one reference per gather, the downslope texels of a sloped
  receiver counted as blockers: lit slopes missed the early-out and the blocker distance came out
  short (narrower penumbrae on slopes).
- Search coverage is less than master's. Master's 8 reads sit at 0.47-1.07 x the search radius in 8
  directions; the gathers cover the centre and one ring. A thin occluder (branch, pole, railing)
  that lies only in the annulus between ring points is missed, the early-out returns 1.0 and its
  outer penumbra is cut off. The map is texel-snapped, so it does not crawl. A second ring of 4
  gathers at 0.5 x the radius, rotated 45 degrees, is the fix if such leaks get reported; it adds
  4 gathers and was not timed, so this PR ships without it.

The penumbra width now follows master's within the coverage limit above; the visible difference is
sampling noise inside penumbrae (8 hardware 2x2-PCF taps instead of 16) and the lit early-out.

Rejected variants (same build, in-run toggles, run `mine1`/`mine2`): a fixed-radius 8-tap PCF
with no search was 0.5-1.0 ms slower than the gather search (it pays 8 taps on every lit pixel);
master's 9-tap search with 8 compare taps saved 0.3-0.9 ms less (3.6 at the snow field); moving
the gather ring to 0.7 of the search radius cost the same and looked the same (A/B within 1 %).

**Item B, first cascade at 2048.** The map stays one 4096 array; cascade 0 is drawn into a 2048
corner of its layer (viewport 2048, texel 2 x radius / 2048, about 1 cm; the layer is still
cleared whole, so taps past the corner read "lit"), and receivers scale that cascade's uv by
`ws_par.z` = 0.5. Only on a 4096 map (the high tier); medium (2048 x 3) and low are unchanged.
Cascade 0 is where most receiver pixels land at close range, and its blocker search spans about
38 texels at 0.5 cm (a 16 m penumbra reach); at 1 cm the same reach is 19 texels, so the texture
cache does better. The saving is mostly receiver-side (tie and tfrag groups), not the prepass.
In the #393 cascade cache the drawn size is `csize[c]` and the redraw test compares it, so
flipping the bit redraws cascade 0.

Bias: the normal offset, the constant bias and the receiver-plane slope allowance all stay at the
full-size (4096) texel (`bias_texel = texel * par.z`, slope term `* par.z`); doubling them with the
2048 texel opened a lit sliver along contact creases. Three variants were shot item-B-only at
jungle (crease crop at 2857, 1991): bias doubled with the texel, max 93 and 2181 px over 8; offset
and constant at the 4096 texel, 79 and 1582; plus the slope term at the 4096 texel (shipped), frame
max 74. The sliver is smaller, not gone: a known issue, filed for after release (`kb/symptoms.md`).

## Timing

`sandover-perf` with `perf-max.yaml` (MSAA 2 + SMAA, the manifest default since #390, both arms;
vista wait 180 s), 45-frame windows, A/B/A, lanes held before boot, solo GPU. Base = staged master
bin at engine c33058c53 (`20261010-154105-200d18e5a9`); branch = that bin with this branch's gk.
Both arms ran with the same `$SANDOVER_DATA`; the base ran from the main checkout as project root,
the branch from a staged root copy (worktree `game/`, the main checkout's `out/jak1`, `goal_src`,
`custom_assets`, `decompiler`, plus `sandover/data/{lighting,sky,materials}` for the final runs),
and the two runs are 10 hours apart, so the cross-run frame column carries drift; the in-run bit 28
toggle is the number to read. `wait.ok` true at every spot in both arms except snow-switch-field,
where the village3 neighbour never finishes loading in either arm (same scene both ways, so its
in-run bit 28 total is usable).

The numbers were measured on the first version of the shader. The review fixes and the bias change
were not re-timed: the gk C++ is the same, and so is the fetch count per pixel (5 gathers and 8
compare taps in the outer cascades, cascade 0 unchanged). The fixes add a few ALU ops per gather,
and the per-texel reference lets more lit slopes take the early-out.

| spot | base frame ms | branch frame ms | bit 28 off costs (in-run) | item A (outer filter) | item B (rest) | world-map rx base | world-map rx branch |
|---|---|---|---|---|---|---|---|
| beach-shore | 29.47 | 27.86 | 3.10 | 2.06 | 1.04 | 5.96 | 3.33 |
| jungle | 32.01 | 27.23 | 4.67 | 3.01 | 1.66 | 12.94 | 7.15 |
| swamp-dock1 | 29.97 | 26.93 | 4.45 (GPU 6.17) | 3.30 | 1.15 | 10.53 | 7.16 |
| village1-trees-1500 | 29.75 | 28.73 | 3.07 | 1.51 | 1.56 | 7.36 | 5.51 |
| beach-whole-1730 | 26.34 | 24.21 | 3.91 | 1.65 | 2.26 | 5.64 | 4.04 |
| jungle-moon-2300 | 36.69 | 29.46 | 4.97 | 3.11 | 1.86 | 13.63 | 7.63 |
| snow-switch-field | 19.80 | 17.02 | 3.41 | ~ (noise) | ~ | 6.39 | 4.24 |
| lavatube-field-1200 | 22.74 | 18.06 | 3.88 | 2.48 | 1.40 | 8.75 | 5.08 |

Bit 28 saves 3.1-5.0 ms of frame at every spot (run `final1`, `--custom bit28 clear:28
mask:0xffffffff`); the world map receivers' marginal falls by a third to a half. Item A is run
`mine2`'s outer-filter toggle (frame ms, first cascade held at 2048); item B is the total minus
item A, 1.0-2.3 ms. The two runs are separate windows, so the split is indicative; the total is
not. At snow-switch-field the `mine2` window noise was 6.4 ms, so the split there is not usable.
The night spot (jungle-moon-2300, the moon casting) gains as much as the day spots.

After the rebase onto #393 (prepass cadence, bit 30) the bit was re-timed at two spots with the
harness's static-window mask (bit 30 cleared, so every cascade redraws as in play; run `rebased1`,
`--toggles opt-bit28`): jungle 5.47 ms, beach-whole-1730 4.15 ms (of which the `world-shadows`
prepass group 0.91: cascade 0 is rasterised at 2048). Use `--toggles opt-bit28` for any re-time:
the restore form `mask:0xffffffff` would turn the bit 30 cache on for the after-base window.

## Look

Paused-frame A/B (`capture run --ab-mask 0xffffffff 0xefffffff`, actors frozen, same frozen frame
rendered A, B, A2), 8-bit PNG, |A-B| max over channels, first version of the shader:

| spot | max | p99 | p99.9 | px > 8 | px > 2 | floor (A vs A2) max |
|---|---|---|---|---|---|---|
| beach-shore | 109 | 0 | 20 | 0.285 % | 0.551 % | 1 |
| jungle | 169 | 9 | 28 | 1.012 % | 2.831 % | 0 |
| swamp-dock1 | 29 | 1 | 3 | 0.004 % | 0.174 % | 0 |
| village1-trees-1500 | 113 | 5 | 27 | 0.571 % | 1.815 % | 0 |
| beach-whole-1730 | 119 | 1 | 5 | 0.033 % | 0.334 % | 0 |
| jungle-moon-2300 | 34 | 3 | 7 | 0.059 % | 1.229 % | 0 |
| snow-switch-field | 11 | 0 | 0 | 0.000 % | 0.000 % | 0 |
| lavatube-field-1200 | 21 | 0 | 1 | 0.002 % | 0.022 % | 57 (0.0007 %) |

Item B with the shipped bias, switched alone (item A disabled in the shader, run
`ab28-half0-slope`): jungle max 74, p99 1, 0.205 % over 8; village1-trees-1500 105, p99 2,
0.259 %; swamp-dock1 21, 0.001 %; floor 0 at all three. No A/B of the combined final shader was
shot. Item A's look evidence is the first-version item-A-only A/B (`outerv4-*`, max 109-119 on
0.03-0.66 % over 8), from before the zero-mean kernel and the per-texel reference.

What the differences are (crops in `shadow-kernel-1/`, index.md, and
`r2/` for the bias variants):

- Item B: texel-scale detail in near shadows. A sunlit sliver along a rock crease near the camera
  at jungle (known issue, above); leaf self-shadow stripes on the grass by Jak at village1 move
  within the blades; a reed's shadow edge at swamp-dock1 moves by a pixel; no thickening at the
  swamp actor contact. No aliasing or stair-stepping on the shadow edges at 4K.
- Item A: penumbra speckle in foliage and dappled canopy shadow at 15-40 m (same pattern, different
  noise), thatch self-shadow at 20-40 m. The first version's dimmer sunlit sand rim at beach-shore
  (-9 luma on 0.4 % of the frame) fits the off-centre kernel the review found, now fixed.
- Cascade boundary: a 16-frame pan at village1-trees-1500 walking 0.3 m per frame sweeps the big
  ground shadow edge through 5-12 m, across the 10 m boundary; bit on and bit off look the same
  frame by frame (`pan-boundary-sheet.png`): no seam, no shimmer, same edge softness. Whole-frame
  frame-to-frame change is the same in both arms (15.3-18.1 mean luma per frame on vs 15.4-18.2
  off), so the 2048 first cascade adds no visible crawl in motion.

## Process

The second pass waited on the shared build lanes: 2 h 10 min and eight lost turns (another
waiter took the lane each time it freed) before its item-B A/B got them. Taking both lanes in
parallel at a 50 ms poll fixed it.

Not done here: an exact cheaper path (a max-depth mip of the map for an all-blocked early-out)
needs the prepass side.
