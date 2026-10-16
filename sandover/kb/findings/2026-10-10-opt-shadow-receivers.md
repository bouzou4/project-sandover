# World map receiver lookups skipped where they cannot change the colour (2026-10-10)

Items (a)1-3 of `2026-10-10-perf-profile.md`. One change landed (item 1), behind perf-opt bit 31
(`PERF_OPT_CORE + 3`, uniform `ws_gate`, default on); item 2 had nothing exact left to take; item
3 is deferred. No setting, tier or look moves.

## Setup

One engine build for both arms (master 2b2b3fa74); only the shaders differ (`--proj` the
worktree, master's shaders for the base). 3840x2160, MSAA 4 plus SMAA (the profile's setting, not
the 2x default of #390), every row at its top tier, `perf-max.yaml` with the vista wait at 180 s.
Base and branch measured back to back in one build-lane hold, solo GPU, 45-frame windows. Every
spot's vista wait finished (`wait.ok` true) in both arms.

Look: paused-frame A/B (`sandover-capture run --ab-mask 0xffffffff 0x7fffffff`, actors frozen,
same settings): each spot renders one paused frame with bit 31 on (A), off (B) and on again (A2).
B equals A2 on every pixel at all eight spots, PNG and EXR (max 0). A differs from both at three
spots (citadel 768 px up to 7, village2 261 px up to 1, jungle 23:00 28 px up to 1) with the mask
unchanged between A and A2, so it is the rig's floor, not the bit (`ab31/pixdiff-B-A2.json`: B vs
A2 max 0, EXR 0). Hypothesis for the floor: the tile-light hysteresis and fades still settling on
the first paused render, which `RIG_AB_SETTLE` (rig.gc) covers for the later renders only; the
pixels cluster mid-frame at an 8-bit level of about 40 (citadel x 1212-2508, y 524-948), where
the lamps light the stone.

## Item 1: skip the lookup where it cannot change the colour (landed)

`ws_removed` is `max(share - (1 - occ) * share_lit, 0)`, and `share_lit` is never negative, so
where the bake's direct share is 0 the removed share is 0 whatever the map says. The map's
occlusion reaches the colour by two more paths only: the PBR sun term (`pbr_sun_occ`: the snow
glints and the wet-film sun lobe) and the debug views. A receiver now always looks the map up in a debug
view, and otherwise only where `shadow_receive * shadow_strength` is non-zero, the actor map has
not already fully occluded the point, and either share > 0 or a PBR sun term can run at this pixel (`pbr_sun_occ_read`: a glint class with a sun share, rain on a porous class, or a porous
class within reach of the shore band or splash zone above sea level; the reach bounds
`sw_shore_band` and `sw_splash_zone` from above, so the skip is exact). Wind TIE reads the map
only through `shadow_strength * shadow_share1 * occ`, so it skips where that product is 0.

Where it pays: receivers the bake keeps out of the direct light, which is most of the frame at
night and indoors (village2 at 03:09, the jungle under the moon, the dark cave, the citadel). A
first version that deferred the PBR term's lookup into `pbr_background` (also exact) made
swamp-dock1 1.2 ms slower: the second call site inlined a second copy of the soft-shadow code into
every background shader. The per-pixel predicate keeps one call site.

## Item 2: early-outs in the soft shadow (nothing exact left)

Master already takes one cascade outside the blend band (the blend weight reaches 1 and the loop
skips the next cascade) and leaves after 4 compare taps when no blocker tap is blocked. The
remaining case, all 9 blocker taps blocked, is not exact: the 16 compare taps sit between the
blocker taps and see the gaps in foliage and fences that the 9 taps miss. Returning 0 there saved
a further 1.7-6.6 ms of the world-map marginal but changed the look: village2 p99 155 (8-bit) on
the pixels the two base runs agree on, jungle at 23:00 110 k pixels over 2. Dropped. An exact
version needs a max-depth mip of the world map (prepass side, `opt/shadow-prepass`).

## Item 3: one lookup per pixel (deferred)

Needs a depth prepass of tfrag, TIE (wind included) and shrub with the colour pass's vertex math
(invariant positions for an equal test), limited to depth-writing unblended draws, alpha-tested
draws with their alpha test, and the colour pass's vis-string culling and LOD; or a conservative
variant that skips the lookup only behind an opaque prepass depth, which under MSAA needs the
per-pixel maximum over samples. Front-to-back order is not exact (depth ties resolve by draw
order). Both are multi-renderer engine changes beyond a receiver pass; recommended as their own
item, sized first with a fragment-invocation count per pixel.

## Results

| spot | base frame ms | branch frame ms | world-map rx base | world-map rx branch | rx saved | paused A/B: bit off vs on, max (px) | floor A vs A2, max (px) |
|---|---|---|---|---|---|---|---|
| beach-shore | 29.01 | 29.85 | 6.68 | 5.67 | 1.01 | 0 (0) | 0 (0) |
| open-sea-low | 27.84 | 27.79 | 0.51 | 0.29 | 0.22 | 0 (0) | 0 (0) |
| jungle | 31.60 | 30.85 | 11.93 | 11.04 | 0.89 | 0 (0) | 0 (0) |
| swamp-dock1 | 29.84 | 30.00 | 10.62 | 10.65 | -0.03 | 0 (0) | 0 (0) |
| village2-night-lamps | 38.52 | 30.26 | 13.39 | 8.44 | 4.95 | 0 (0) | 1 (261) |
| darkcave-0025 | 24.52 | 20.22 | 10.64 | 6.25 | 4.39 | 0 (0) | 0 (0) |
| jungle-moon-2300 | 35.12 | 32.89 | 14.06 | 11.75 | 2.31 | 0 (0) | 1 (28) |
| citadel-lamps-0600 | 22.43 | 21.46 | 8.95 | 7.84 | 0.5-1.1 | 0 (0) | 7 (768) |

The world-map marginal is measured inside each run (A/B/A), so it is the number to read; frame
times drift by up to 2 ms between runs (village2's base frame read 34.7 in an earlier base run and
38.5 here). Citadel's base marginal read 8.38-8.95 across four base runs, so its saving is 0.5-1.1
ms against that spread. Bit 31 on the shipped form (staged branch bin, village2-night-lamps, one
A/B/A toggle, mask all on against bit 31 cleared): clearing the bit costs 4.93 ms of frame (tie
3.01, tfrag 1.75), the same as the shader-only measurement. The table's timing was taken on the
shader-only form of the change (no bit; the bit adds one int uniform test).
