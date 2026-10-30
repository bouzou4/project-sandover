---
title: What makes the ocean read as a grid from high up, and what removes it?
date: 2026-09-26
status: inferred
sha: 061cd492b (base; branch water-antitile)
---
## Answer (≤5 lines)
There are two repeats, and the texture one is the stronger. The game's ocean texture is one
tile every 96 m, and the shading takes its colour, ripple normal and crest sparkle from it.
The five Gerstner waves are also near-commensurate, so their crests realign. Fix: hex tiling
of the texture in world space, a two-octave crest warp per wave, and wave lengths that are
not in small ratios. Offline prediction: the 96 m texture peak drops from 1.00 to 0.04, and
the wave-pattern peak at 100-200 m drops from 0.86 to 0.48.

## Evidence
- Texture repeat: `render-ocean-far` gives each 393216-unit (96 m) quad st 0..1
  (goal_src/jak1/engine/gfx/ocean/ocean.gc:55-72@061cd492b; 15 repeats over 1440 m). The
  near ocean steps st by 0.25 per 24 m tile (ocean-near.gc:426). The mid uses the same
  corner table (`*ocean-trans-st-table*`, ocean-mid.gc:549).
- Capture autocorrelation. Method: rectify onto the y = 0 plane (64° horizontal FOV, square
  pixels), band-pass at 40 m, then normalized ACF over the ocean pixels.
  - `$SANDOVER_DATA/experiments/water-camera/water/temple-top-dusk.png` (156 m up), sea window 340-800 m
    ahead: strongest repeat 0.69 at 99.7 m, with 0.47 at 97.2 m (the 96 m tile; the few
    percent of skew is FOV error). The tile's lattice ghosts come next: 0.43 at 80.6 m and
    0.42 at 50.4 m.
  - `open-sea-seam-dusk.png` (12 m up): 0.57 at 11.8 m, with harmonics at 24.2 m and
    36.2 m. This is the main wave, foreshortened. From this low the texture tile does not
    dominate.
  - `beach-shore-afternoon.png` (2.8 m up): 0.50 at 40 m. At this height the plane
    rectification is unreliable (the waves occlude each other), so this one is not used.
- Offline model (numpy port of `water_gerstner`, 800 m square at 0.5 m, slope field along
  x, which the shading reads):

  | ACF peak | revision-5 bold | this branch |
  |---|---|---|
  | 40-100 m | 0.94 at 45 m | 0.72 at 47 m |
  | 100-200 m | 0.86 at 101 m | 0.48 at 104 m |
  | 200-400 m | 0.82 at 385 m | 0.35 at 395 m |
  | at revision 5's six strongest lags (45-54 m) | 0.92-0.93 | 0.32-0.57 |

  Under 50 m the crests stay coherent on purpose: that is what a wave field looks like. What
  goes away is the long-range realignment that reads as a grid.
- Texture model: a random 128² tile with a red spectrum, repeated every 96 m, has an ACF of
  1.00 at 96 m. Hex tiled at 40, 56 and 72 m with rotation up to ±180°, its strongest peak
  beyond 20 m is 0.04 (at 189, 153 and 114 m).
- Steepness: the warp shifts each wave's phase gradient by at most 3 · 1.866 · warp_m /
  warp_cell_m across the wave (0.31 at 10 m / 180 m), and a third of that along it. Bold's
  steepness sum stays at 3.73, under the chop's divisor of 4, so crest sharpness is unchanged
  from revision 5. The worst case, where every term aligns, is 0.93, so crests never fold.

## Implications for the design
- The detail texture's repeat is the one to fight first from high vantage points. The
  Gerstner set only needs to stop realigning, not to become spectral.
- The far band (vanilla DirectRenderer quads) keeps the game's repeat. Hex tiling fades out
  with the pre-shoal wave weight at the mid's outer edge, so the seam is the same flat
  crossfade as before.

## Follow-ups
- Solo GPU cost of the mid and near ocean at 1440p MSAA 4, bold vs the `no-antitile` capture,
  for the perf pass. Estimate: about 3x the texture fetches on ocean pixels (15 to 45) and
  +5 value-noise evaluations per tessellated vertex and per pixel.
- If a repeat still shows in the A/B, a spectral (FFT) ocean remains an
  optimization-pass option.
