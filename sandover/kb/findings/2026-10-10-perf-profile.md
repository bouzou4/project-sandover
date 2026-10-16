# Max-tier frame profile (2026-10-10)

The maintainer's target: "with all settings maxed we should at least be hitting minimum 4k60 target on a
PS2 (albeit very enhanced) game". This is the profile the optimization authors work from. No engine
code changed; measurement only.

## Setup

- Run `$SANDOVER_DATA/perf/20261010-max2` (`perf.json`, `summary.json`, `perf.md`, a PNG and EXR
  per spot, `contamination.json` with the gk count and build-b state of every timer window). The
  earlier partial run `20261010-max` (2 spots, interleaved with other captures) is not used.
- Build: the staged play bin at engine `7f1efdad6` (master `db450cf2d` adds docs only).
- 3840x2160, MSAA 4x plus SMAA (`antialias` mode 4), every Sandover row on at its top tier
  (`sandover-settings-reset!` defaults: SSAO high, shadows high = world map 4096 x 3 cascades and
  actor map 2048, tile lights, canopy, water, glint, sun, render scale 100 %, FP16, HDR PQ through
  the DXGI flip present), vsync and limiter off, all applied before the first measurement.
- Spots: `sandover/shots/perf-max.yaml`, the 21 spots of `perf.yaml` plus 12 where the features
  merged since 2026-10-08 cost most (cameras from existing frames; sources in the file).
- Command (from `sandover/tools`). The max tier comes from the after-load
  `sandover-settings-reset!` that sandover-perf sends, with `smaa?` set to #t before
  `sandover-settings-reapply` (a wrapper around `sandover.rig.perf`), not from the manifest:
  `sandover-perf sandover/shots/perf-max.yaml --bin <play bin> --toggles ssao tile-lights shadows
  water hdr-output water-tess rs-77-fsr vista --custom world-map "(pc-set-shadow-option 32 0)"
  "(pc-set-shadow-option 32 1000000)" --custom world-2048 "(pc-set-shadow-option 35 2048000000)"
  "(pc-set-shadow-option 35 4096000000)" --custom world-2casc "(pc-set-shadow-option 36 2000000)"
  "(pc-set-shadow-option 36 3000000)" --custom shadow-med "(set! (-> *pc-settings* sandover
  shadow-quality) 1)" "(set! (-> *pc-settings* sandover shadow-quality) 2)" --custom ssao-med
  "(set! (-> *pc-settings* sandover ssao-quality) 1)" "(set! (-> *pc-settings* sandover
  ssao-quality) 2)" --custom smaa "(set! (-> *pc-settings* sandover smaa?) #f)" "(set! (->
  *pc-settings* sandover smaa?) #t)" --custom glint "(set! (-> *pc-settings* sandover glint?) #f)"
  "(set! (-> *pc-settings* sandover glint?) #t)" --custom pan-2deg "(perf-pan-start 364.0)"
  "(perf-pan-stop)"`. `perf-pan-start` / `perf-pan-stop` are two GOAL defuns the wrapper sends at
  boot: a process that composes a world-up rotation of `a += step` with `*rig-opts* cam-rot` every
  frame and calls `debug-set-camera-pos-rot!`; stop restores the rig camera.
- The shipped default is now MSAA 2x plus SMAA. This run used MSAA 4x; 4x against 2x was not
  measured (the SMAA marginal sits on top of MSAA 4x).
- Since #390 `perf-max.yaml` runs MSAA 2x: reproduce this run with `--msaa 4` added to the
  command. The wrapper's `smaa?` #t step is a no-op since then (the reset sets it).
- 45-frame windows, A/B/A per toggle, solo (no other gk in any window; the `build` lane held for
  the whole run), 81 min for 33 spots.
- Toggles: the effect rows (`ssao` includes the tile lights), the probes `water-tess` and
  `rs-77-fsr`, and customs: `world-map` (shadow option 32 = 0: world receivers stop reading the
  map; the map is still drawn for actors and water), `world-2048` (option 35), `world-2casc`
  (option 36), `shadow-med` and `ssao-med` (each row's medium tier), `smaa` (off), `glint` (off),
  `pan-2deg` (a process turns the rig camera 2 degrees per frame about world up for the window).
- **The vista wait timed out at 10 of 33 spots** (90 s for the neighbour loader; `wait.ok` false in
  `perf.json`; village2 and rolling, or the snow neighbours, still loading): beach-shore,
  village1-night, jungle, low-behind-jak, snow, village1-hut-lamps, village1-trees-1500,
  beach-whole-1820, jungle-moon-2300, snow-switch-field. Four of the worst five are among them.
  Those frames may lack some neighbour draws, so their frame and vista costs may be understated;
  several have tight bands, so the noisy label does not flag them. They are marked (w) below.
  `perf-max.yaml` now waits up to 180 s.
- The world map pass logs its own GPU and CPU time every 120 frames (`world-shadows: ... pass gpu
  ... cpu`); per-spot medians are under "World map pass".

Reading: a marginal is the frame time saved with the toggle off (positive = the feature costs that
much); `~` marks a value under half the spot's noise band (perf.md marks values inside the whole
band, so it shows more `~`). Spots marked noisy have a base-window band over 5 ms (7 spots:
village1-night, jungle, sunken, beach-windmill-0700, training-shore-1200, snow-switch-field,
swamp-eco-blob). **Every median and maximum in this doc is over the other 26 spots**; the noisy
spots appear in the per-spot tables only.
The pan window turns through every direction, so its marginal mixes view cost with motion cost;
read the pan from its frame maxima (below), not its marginal.

## Result

Mean frame 26.4 ms over 33 spots (38 fps). Two spots meet 16.7 ms: **finalboss 7.9** and
**maincave-crystals 14.6**. Nearest: maincave 18.7, village1-waterfront 19.0, darkcave 19.9.

Every spot is GPU-bound. The strongest evidence: with the tile lights off (no per-frame candidate
readback, which is a full CPU-GPU sync) the EE and render-thread times fall by a median 12.2 ms
while the frame falls by 1.0 ms, so the CPU has over 10 ms of headroom and the game thread (EE) is
not the limiter at any spot at this tier. The GPU toggles move the frame one for one (77 % FSR: EE
over frame marginal 0.97). The CPU side shows only as hitches while panning (below).

Against the 2026-09-28 shipping numbers (deep-opt; MSAA 4, no SMAA) the same spots are 9-25 ms
slower: jungle 10.5 -> 34.5, darkcave 6.9 -> 19.9, village2 night 11.8 -> 36.7. Most of it is the
shadow work merged since: the shadows row costs a median 10.3 ms (max 20.7), of which the
world map's receivers are 6.5.

### Worst five

| spot | frame ms | worst passes (GPU ms) | world map receivers | whole shadows row | gap to 16.7 |
|---|---|---|---|---|---|
| village1-night 23:00 (noisy, band 22 ms; w) | 40.1 | vista 11.2, tfrag 7.7, tie 6.5 | 7.6 | 12.7 | +23.4 |
| jungle-moon-2300 (w) | 39.1 | tie 16.7, tfrag 8.0 | 14.5 | 20.7 | +22.4 |
| village1-trees-1500 (w) | 37.0 | tfrag 8.5, tie 6.3, actor-shadows 4.9 | 8.7 | 14.6 | +20.3 |
| low-behind-jak (w) | 36.9 | tie 8.1, vista 7.2, tfrag 5.0 | 7.2 | 12.4 | +20.2 |
| village2-night-lamps 03:09 | 36.7 | tie 10.8, tfrag 6.6, actor-shadows 4.1 | 13.5 | 18.6 | +20.0 |

### Per spot

"world map rx" is the `world-map` marginal (receiver lookups), "shadows" the whole row (receivers,
world map pass, actor sun and spot maps, local lights), "vista" the neighbour levels.

| spot | hour | frame ms | GPU ms | noise | worst pass (ms) | next (ms) | world map rx | shadows | vista | gap to 16.7 |
|---|---|---|---|---|---|---|---|---|---|---|
| beach-shore (w) | 15 | 32.2 | 32.3 | 1.8 | tie 6.8 | vista 6.1 | 6.7 | 12.1 | 8.6 | +15.5 |
| open-sea-low | 18 | 29.2 | 29.2 | 0.1 | vista 14.8 | ocean-mid 6.0 | 0.4 | 4.3 | 15.2 | +12.5 |
| open-sea-high | 12 | 21.6 | 21.6 | 0.8 | vista 12.4 | actor-shadows 2.0 | 0.7 | 3.5 | 12.8 | +4.9 |
| village1-night (noisy) (w) | 23 | 40.1 | 40.1 | 22.1 | vista 11.2 | tfrag 7.7 | 7.6 | 12.7 | 13.2 | +23.4 |
| hut-roof | 12 | 29.8 | 29.8 | 1.4 | vista 13.8 | tie 4.0 | 2.7 | 6.7 | 15.0 | +13.1 |
| jungle (noisy) (w) | 12 | 34.5 | 34.5 | 8.0 | tie 12.8 | tfrag 6.8 | 12.0 | 17.2 | -0.0 | +17.8 |
| temple-top | 18 | 21.2 | 21.2 | 1.8 | vista 5.9 | tie 2.5 | -0.0 | 3.4 | 6.4 | +4.5 |
| low-behind-jak (w) | 12 | 36.9 | 36.9 | 2.9 | tie 8.1 | vista 7.2 | 7.2 | 12.4 | 7.7 | +20.2 |
| sunken (noisy) | 12 | 22.1 | 22.2 | 18.2 | tfrag 7.8 | actor-shadows 4.8 | 5.2 | 9.5 | -0.0 | +5.4 |
| maincave | 22 | 18.7 | 18.7 | 4.9 | tfrag 7.9 | actor-shadows 3.2 | 5.9 | 7.5 | -0.0 | +2.0 |
| darkcave | 12 | 19.9 | 19.9 | 0.5 | tfrag 9.5 | actor-shadows 3.6 | 6.3 | 9.0 | 0.0 | +3.2 |
| snow (w) | 12 | 21.6 | 21.6 | 2.2 | tfrag 5.2 | tie 5.0 | 5.8 | 9.7 | 0.1 | +4.9 |
| swamp | 14.819 | 20.5 | 20.5 | 0.3 | tfrag 4.7 | tie 4.7 | 5.3 | 8.2 | 0.1 | +3.8 |
| swamp-dock1 | 14.819 | 31.5 | 31.5 | 0.2 | tie 12.6 | tfrag 6.1 | 10.6 | 13.9 | 2.6 | +14.8 |
| village1-hut-lamps (w) | 22.4819 | 29.9 | 29.9 | 3.2 | tfrag 12.0 | tie 5.9 | 11.3 | 16.2 | 1.2 | +13.2 |
| village2-night-lamps | 3.1503 | 36.7 | 36.7 | 0.2 | tie 10.8 | tfrag 6.6 | 13.5 | 18.6 | 2.4 | +20.0 |
| maincave-crystals | 12.5508 | 14.6 | 14.6 | 0.3 | tfrag 6.0 | ssao 1.7 | 3.9 | 5.3 | 0.2 | -2.1 |
| darkcave-0024 | 14.9664 | 23.6 | 23.6 | 0.1 | tfrag 12.1 | actor-shadows 3.1 | 9.4 | 11.4 | -0.0 | +6.9 |
| darkcave-0025 | 12.2589 | 26.0 | 26.0 | 0.3 | tfrag 13.4 | actor-shadows 3.4 | 10.6 | 13.3 | -0.2 | +9.3 |
| ogre | 12 | 22.4 | 22.4 | 0.6 | tie 9.0 | tfrag 5.5 | 8.8 | 10.8 | 0.2 | +5.7 |
| finalboss | 12 | 7.9 | 7.8 | 0.1 | tfrag 1.7 | etie 1.2 | -0.0 | 0.3 | 0.1 | -8.8 |
| village1-trees-1500 (w) | 15 | 37.0 | 36.9 | 1.8 | tfrag 8.5 | tie 6.3 | 8.7 | 14.6 | 6.3 | +20.3 |
| beach-whole-1730 | 17.5 | 33.8 | 33.8 | 0.7 | actor-shadows 8.0 | tfrag 8.0 | 7.0 | 15.8 | 2.3 | +17.1 |
| beach-whole-1820 (w) | 18.3333 | 23.1 | 23.1 | 1.1 | tfrag 5.6 | tie 3.2 | 4.3 | 7.7 | 1.7 | +6.4 |
| jungle-moon-2300 (w) | 23 | 39.1 | 39.1 | 3.0 | tie 16.7 | tfrag 8.0 | 14.5 | 20.7 | 0.0 | +22.4 |
| beach-windmill-0700 (noisy) | 7 | 30.4 | 30.3 | 7.2 | actor-shadows 6.1 | tfrag 5.9 | 6.1 | 13.5 | 4.7 | +13.7 |
| village1-waterfront-1200 | 12 | 19.0 | 19.0 | 1.1 | ocean-mid 5.5 | ocean-near 3.9 | 1.6 | 5.0 | 0.4 | +2.3 |
| jungle-river-0800 | 8 | 23.8 | 23.8 | 0.3 | tfrag 5.4 | tie 4.3 | 6.0 | 11.2 | 3.1 | +7.1 |
| training-shore-1200 (noisy) | 12 | 21.5 | 21.5 | 13.4 | tie 8.0 | tfrag 3.2 | 5.9 | 8.7 | 1.0 | +4.8 |
| citadel-lamps-0600 | 6 | 22.9 | 22.9 | 1.2 | tie 13.8 | ssao 1.7 | 8.7 | 11.7 | -0.3 | +6.2 |
| snow-switch-field (w) | 15.668 | 23.3 | 23.3 | 5.1 | tfrag 6.1 | tie 5.9 | 7.8 | 13.0 | 0.1 | +6.6 |
| swamp-eco-blob (noisy) | 14.819 | 33.7 | 33.7 | 10.3 | tie 12.1 | tfrag 7.5 | 10.4 | 14.8 | 4.9 | +17.0 |
| lavatube-field-1200 | 12 | 22.0 | 21.9 | 0.3 | tie 7.7 | tfrag 4.8 | 7.7 | 9.7 | -0.1 | +5.3 |

### Marginals (frame ms saved with the toggle off)

| spot | ssao | tile-lights | shadows | water | hdr-output | water-tess | rs-77-fsr | world-map | world-2048 | world-2casc | shadow-med | ssao-med | smaa | glint | pan-2deg | vista |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| beach-shore (w) | 2.94 | 1.80 | 12.11 | 3.20 | 0.23~ | 0.82~ | 5.78 | 6.69 | 1.90 | 0.83~ | 1.84 | 0.25~ | 0.69~ | 0.24~ | 12.43 | 8.56 |
| open-sea-low | 1.54 | 0.75 | 4.31 | 7.86 | 0.10 | 4.99 | 4.10 | 0.44 | 0.92 | 0.28 | 0.24 | 0.04~ | 0.84 | 0.14 | 9.51 | 15.18 |
| open-sea-high | 1.88 | 0.74 | 3.46 | 1.13 | 0.12~ | 0.23~ | 2.53 | 0.74 | 0.70 | -0.22~ | -0.43 | 0.04~ | 0.71 | 0.03~ | 12.12 | 12.77 |
| village1-night (w) | 1.90~ | 1.03~ | 12.67 | 0.60~ | 0.16~ | 0.56~ | 6.10~ | 7.57~ | -7.80~ | 10.53~ | -17.37 | 0.40~ | 0.89~ | 0.01~ | 20.41 | 13.15 |
| hut-roof | 1.98 | 0.82 | 6.69 | 1.40 | 0.18~ | 0.70~ | 3.49 | 2.69 | -1.15 | 0.82 | 0.73 | 0.04~ | 0.82 | 0.03~ | 16.57 | 15.02 |
| jungle (w) | 5.73 | 3.17~ | 17.15 | 0.32~ | 0.17~ | 0.05~ | 7.47 | 11.97 | 2.26~ | 1.94~ | 2.41~ | 0.18~ | 0.94~ | 0.01~ | 3.18~ | -0.03~ |
| temple-top | 2.83 | 3.45 | 3.43 | 2.48 | 1.64 | 0.16~ | 5.66 | -0.01~ | -1.07 | 0.58~ | 0.20~ | 0.95 | 0.18~ | 0.14~ | 6.56 | 6.40 |
| low-behind-jak (w) | 4.21 | -0.31~ | 12.41 | 1.74 | -0.11~ | 0.82~ | 6.75 | 7.16 | 1.52 | 0.37~ | 1.77 | 0.19~ | 0.88~ | 0.06~ | 25.52 | 7.73 |
| sunken | 2.80~ | 0.92~ | 9.47 | 0.04~ | 0.22~ | -0.01~ | 4.25~ | 5.20~ | 11.66 | 10.31 | -11.23 | 3.52~ | 0.91~ | 0.02~ | 1.62~ | -0.01~ |
| maincave | 3.25 | 1.21~ | 7.53 | -0.02~ | 0.14~ | 2.48 | 6.63 | 5.88 | 1.46~ | 1.63~ | 1.53~ | 0.47~ | 1.04~ | 0.04~ | 2.10~ | -0.01~ |
| darkcave | 2.86 | 0.91 | 9.03 | 0.01~ | 0.16~ | -0.03~ | 4.41 | 6.32 | 1.79 | 0.91 | 1.98 | 0.20~ | 0.77 | 0.05~ | 0.30 | 0.05~ |
| snow (w) | 2.95 | 1.66 | 9.69 | -0.71~ | 0.25~ | 0.51~ | 4.96 | 5.84 | -10.62 | 1.05~ | 0.84~ | 0.05~ | 1.04~ | -1.41 | -11.60 | 0.14~ |
| swamp | 2.86 | 0.78 | 8.18 | 0.40 | 0.29 | -0.15~ | 4.97 | 5.32 | 1.60 | 0.44 | 1.72 | 0.25 | 0.68 | 0.01~ | -3.90 | 0.06~ |
| swamp-dock1 | 3.36 | 0.91 | 13.90 | 0.03~ | 0.09~ | 0.05~ | 7.93 | 10.62 | 2.11 | 0.55 | 2.13 | 0.26 | 0.75 | 0.03~ | 5.10 | 2.64 |
| village1-hut-lamps (w) | 3.80 | 1.26~ | 16.21 | 0.37~ | 1.66 | 1.66 | 7.26 | 11.33 | 2.97 | 3.35 | 3.21 | 0.28~ | 0.78~ | 0.02~ | -0.70~ | 1.18~ |
| village2-night-lamps | 3.57 | 1.21 | 18.63 | 1.57 | 0.24 | 0.84 | 8.86 | 13.49 | 2.88 | 1.33 | 3.02 | 0.25 | 0.93 | 0.02~ | -0.21 | 2.42 |
| maincave-crystals | 3.37 | 1.07 | 5.33 | -0.04~ | 0.18 | 0.02~ | 3.37 | 3.87 | 0.91 | 0.61 | 1.01 | 0.34 | 0.79 | -0.06~ | 0.54 | 0.18 |
| darkcave-0024 | 3.12 | 1.01 | 11.36 | 0.01~ | 0.12 | -0.06~ | 5.90 | 9.35 | 2.01 | 0.86 | 2.13 | 0.17 | 0.67 | -0.05~ | 6.33 | -0.00~ |
| darkcave-0025 | 3.21 | 1.12 | 13.34 | -0.05~ | 0.06~ | 0.03~ | 6.49 | 10.59 | 2.26 | 2.69 | 2.37 | 0.23 | 0.66 | 0.01~ | 8.65 | -0.21 |
| ogre | 3.02 | 1.02 | 10.80 | 0.12~ | 0.21~ | -0.02~ | 5.76 | 8.78 | 1.52 | 0.47 | 1.40 | 0.12~ | 0.80 | 0.10~ | 8.12 | 0.24~ |
| finalboss | 1.68 | 0.70 | 0.30 | -0.04 | 0.07 | -0.04 | 1.33 | -0.03~ | -0.04 | -0.02~ | 0.04 | 0.08 | 0.71 | 0.03~ | -2.54 | 0.06 |
| village1-trees-1500 (w) | 3.16 | 1.30 | 14.60 | 3.39 | 1.17 | 1.36 | 6.32 | 8.68 | 2.37 | 1.34 | 2.44 | 0.22~ | 0.85~ | 0.01~ | 5.82 | 6.28 |
| beach-whole-1730 | 2.91 | 1.16 | 15.77 | 5.69 | 0.08~ | 2.91 | 5.45 | 6.97 | 4.21 | 3.58 | 4.28 | 0.34~ | 0.98 | -0.10~ | -0.84 | 2.25 |
| beach-whole-1820 (w) | -0.36~ | 1.16 | 7.74 | 4.08 | 0.04~ | -0.93 | 4.67 | 4.27 | -3.27 | 0.90 | 1.04 | 0.21~ | -0.22~ | 0.09~ | -2.11 | 1.74 |
| jungle-moon-2300 (w) | 5.22 | 2.33 | 20.72 | 0.25~ | 0.25~ | 0.15~ | 8.76 | 14.45 | 3.17 | 2.19 | 3.53 | 0.24~ | 1.10~ | 0.03~ | 9.12 | 0.01~ |
| beach-windmill-0700 | 2.36~ | 0.85~ | 13.52 | 2.64~ | 0.15~ | 1.16~ | 4.53 | 6.10 | 1.17~ | 5.66 | 4.20 | 0.29~ | 1.21~ | 0.15~ | -0.52~ | 4.72 |
| village1-waterfront-1200 | 1.82 | 0.88 | 4.97 | 10.24 | 0.16~ | 3.98 | 4.60 | 1.62 | 0.13~ | 0.68 | 0.46~ | 0.12~ | 1.05 | 0.02~ | 11.67 | 0.40~ |
| jungle-river-0800 | 2.82 | 0.91 | 11.18 | 0.15 | 0.08~ | 0.08~ | 4.99 | 6.00 | 2.38 | 1.77 | 2.33 | 0.24 | 0.84 | 0.17 | -10.86 | 3.13 |
| training-shore-1200 | 3.41~ | 0.69~ | 8.72 | 2.36~ | 0.13~ | 0.50~ | 5.70~ | 5.85~ | 8.15 | 7.01 | 1.21~ | 0.33~ | 1.01~ | 0.04~ | 6.34~ | 0.96~ |
| citadel-lamps-0600 | 3.61 | 1.63 | 11.72 | -0.03~ | 0.01~ | 0.11~ | 5.85 | 8.72 | 1.82 | 2.37 | 2.10 | 0.32~ | 0.40~ | -0.21~ | -8.22 | -0.30~ |
| snow-switch-field (w) | 2.62 | 0.96~ | 13.02 | 0.15~ | 0.19~ | -0.08~ | 5.24 | 7.84 | 4.33 | 6.69 | 4.11 | 0.14~ | 0.81~ | -0.15~ | -7.22 | 0.12~ |
| swamp-eco-blob | 3.61~ | 0.94~ | 14.81 | 3.45~ | 3.04~ | -0.78~ | 7.40 | 10.42 | 2.19~ | 2.38~ | 2.24~ | 0.22~ | 0.66~ | 0.01~ | 10.64 | 4.90~ |
| lavatube-field-1200 | 3.27 | 1.01 | 9.72 | -0.06~ | 0.15 | 0.13~ | 5.57 | 7.73 | 1.84 | 1.37 | 2.08 | 0.13~ | 1.02 | -0.04~ | 2.01 | -0.06~ |

### World map pass (prepass; gk log medians at 4096 x 3)

GPU 0.9-4.4 ms (sunken 4.4, snow 4.1, village1-hut-lamps 3.7, snow switch field 3.6, beach-shore
3.4), CPU 0.3-1.0 ms on the render thread, 190-1260 draws and 150-1200 k triangles per frame over
the three cascades. The fade is 1.00 at every spot except open sea at 18:00 (0.22) and the beach
at 18:20 (0.90): the map is drawn and sampled at full strength in the interiors (sunken, maincave,
darkcave, citadel, lava tube, ogre) and at night (village1 23:00, jungle 23:00, village2 03:09,
where the moon is the shadow light, #375). The first frame after a level load spends about 1 s of
render-thread time building the level's caster cache (`WorldShadows::build`). At low sun
(beach-whole-1730) `actor-shadows` is the worst pass at 8.0 ms and the shadows row costs 15.8.

### Fast pan (2 degrees per frame)

The GPU stays the limiter on average, but the CPU frame maximum spikes well past the GPU's
during the pan: village2-night-lamps 65.6 ms CPU vs 39.9 GPU, jungle-river 62.2 vs 36.8, snow
switch field 55.8 vs 31.7, village1-trees 51.9 vs 32.5, jungle-moon 50.5 vs 38.6, swamp 43.3 vs
26.9. These are render-thread hitches the GPU did not cause. Candidates: the per-frame cull and
multidraw rebuilds over newly visible trees; the vista neighbour draws (the pan windows'
`vista-cpu-ms` maxima track the spikes: village2-night-lamps 60, jungle-river 56, jungle-moon 45,
village1-trees 43, swamp 38; the snow switch field draws no neighbours); and the tile lights'
per-frame readback sync, which stops the CPU and GPU from overlapping. Not split further.

## Costs ranked across spots

| # | cost | file: function | frame ms (median / max, 26 spots) |
|---|---|---|---|
| 1 | World map receiver lookup: per fragment a 9-tap blocker search plus 16 compare taps per cascade, two cascades in the blend bands, in every background fragment, overdrawn ones and ones whose bake share is 0 included | `shaders/world_shadow_frag.glsl`: `ws_vis_cascade`, `ws_sun_vis_c`; called from `main` in `tfrag3.frag`, `shrub.frag`, `etie.frag`, `etie_base.frag`, `tie_wind.frag` | 6.5 / 14.5 |
| 2 | Native resolution over 77 % FSR | every pixel-bound pass | 5.6 / 8.9 |
| 3 | The rest of the shadows row: world map prepass (three cascades redrawn every frame), actor sun map, spot maps, local lights | `WorldShadows.cpp`: `WorldShadows::prepass`, `WorldShadows::draw_level`; `ActorShadows.cpp`: `ActorShadows::prepass` | 3.4 / 8.8 |
| 4 | SSAO with the tile lights | `ssao` group 1.4 ms, `emissive-lights` 0.9 ms (`emlights-tiles` 0.5) | 3.0 / 5.2 |
| 5 | Shadow tier high over medium (world map 4096 -> 2048 alone 1.7 / 4.2; 3 -> 2 cascades 0.9 / 3.6) | `kWorldShadowTier`; `pc_set_shadow_option` QUALITY | 1.8 / 4.3 |
| 6 | SMAA on top of MSAA 4 | `smaa` group (edges, weights, blend) | 0.8 / 1.1 |
| 7 | Vista neighbour levels at the sea and high spots | `vista` group, mostly `vista-part:tie` (open-sea-low 14.8 GPU) | 0.8 / 15.2 |
| 8 | Water at the waterfront and the open sea: ocean-mid 5.5 and ocean-near 3.9 at village1-waterfront, tessellation 5.0 at open-sea-low, the world map lookup per lit water pixel (#372), the near rim rebuilt per frame | `shaders/ocean_common.frag`: `water_shade` (`wsg_sun_vis` -> `ws_water_vis`); `ocean/CommonOceanRenderer.cpp`: `CommonOceanRenderer::near_rim_on_plane` | 0.3 / 10.2 |

Ranked by median, then maximum.

Not measurable at these spots: the glint (`wsg_ripple` in `merc2.frag`, 48 hashes per inland-water
pixel; 0.03 median, 0.24 max), SSAO high over medium (0.23), and the `hdr-output` toggle (0.15;
it compares the DXGI flip present with the GL swap, and the PQ encode stays in `pcrtc` either way).

## (a) Invisible wins (no look change expected)

1. **Skip the world map lookup where it cannot change the colour.** `ws_removed` returns
   `max(share - (1 - occ) * share_lit, 0)`, so with `shadow_share == 0` the colour does not depend
   on the lookup. Gate `ws_world_vis` on `shadow_share > 0` (and on the PBR sun weight, which also
   reads `occ`) in `tfrag3.frag`, `shrub.frag` and `etie*.frag`. The interiors and night spots,
   where the receivers cost 5-14 ms (darkcave-0025 10.6, citadel 8.7, jungle 23:00 14.5), gain most
   if the bake share there is near 0; pixdiff decides.
2. **Early-outs in the soft shadow.** When all blocker taps agree (all lit already returns after 4
   compare taps; all blocked does not), skip the 16 compare taps; take one cascade outside the 10 %
   blend band (the band reads two). Exact where all taps agree.
3. **One lookup per pixel, not per fragment.** The background has no depth prepass, so every
   overdrawn fragment pays the full lookup. A depth-equal pass, or opaque tfrag/tie drawn front to
   back, removes the overdraw share without changing a pixel.
4. **Cascade redraw cadence.** `WorldShadows::prepass` redraws all three cascades every frame. The
   casters are static: redraw a cascade only when its texel-snapped box or the sun moves (exact,
   since the boxes are already snapped).
5. **Per-frame CPU in the world prepass.** `cull_check_all_slow` (the flat cull) plus
   `make_multidraws_from_vis_string` per tree per cascade; one `glUniformMatrix4fv` and one
   `glDrawElements` per wind instance per cascade in `draw_level` (one instanced draw from a matrix
   buffer instead); `glGetIntegerv(GL_DEPTH_FUNC)` and `glGetFloatv(GL_DEPTH_CLEAR_VALUE)` every
   frame (driver syncs; track the state as GlQueries does elsewhere). 0.3-1.0 ms render thread.
6. **Load hitch.** `WorldShadows::build` uploads every tfrag tree's indices a second time into
   `own_index_buffer` (already resident for `TFragment`) and costs about 1 s on the first frame
   after a level load. Reuse the renderer's buffers.
7. **Ocean near rim.** `CommonOceanRenderer::near_rim_on_plane` rebuilds a nested `unordered_map`
   of every near-ocean edge each frame; cache it per index list (CPU).
8. **Water map lookup.** `ws_water_vis` runs per lit water pixel whenever the map is on; test the
   cascade boxes before the taps and skip it where the sun elevation factor `up` is 0.
9. **Pan hitches.** Profile the 50-66 ms CPU frames during pans before choosing a fix; items 5 and
   the vista CPU cost are the candidates, with item 10.
10. **Tile-light readback sync.** The tile lights read a candidate row back every frame, a full
   CPU-GPU sync: switching them off cuts EE and render time by a median 12.2 ms for 1.0 ms of
   frame. An asynchronous readback (the previous frame's candidates) restores the overlap; a
   candidate for the pan hitches.

## (b) Tier choices (top tier over the next one down)

| row | top tier | next tier | frame ms saved (median / max) | note |
|---|---|---|---|---|
| Shadow quality | high: world 4096 x 3, actor 2048, 3x3 PCF | medium: 2048 x 3, 1536, 2x2 | 1.8 / 4.3 | world size alone 1.7 / 4.2 |
| World cascades | 3 | 2 | 0.9 / 3.6 | not a row (option 36) |
| Render scale | 100 % | 77 % FSR | 5.6 / 8.9 | |
| SSAO quality | high (16 taps) | medium (10) | 0.2 / 1.0 | |
| SMAA | on | off | 0.8 / 1.1 | |
| Water tessellation | on | off (probe) | 0.2 / 5.0 | open sea only |
| HDR present | DXGI flip present | GL swap (`hdr-output off`) | 0.15 / 1.7 | the PQ encode stays in `pcrtc` |

Cascade 0's texel is 0.5 cm at 4096 (0.5-10 m from the camera), finer than a 4K pixel there; a
2048 first cascade with 4096 outer ones may keep the look, which needs a look at the rendered frame.

## (c) Look-changing options (need a visual judgement call)

- Fade the world map out where the bake has no direct sun (interiors) and at night, instead of
  running it for the moon at full strength (the receivers cost 14.5 ms at jungle 23:00).
- A cheaper soft shadow: fixed-radius PCF instead of the blocker search, or a smaller kernel in the
  outer cascades.
- Vista LOD by distance (deep-opt plan item; 12-15 ms at the sea and high spots).
- Render scale below 100 % as the max-tier default.
- Deferred ocean shading once per pixel (deep-opt plan item).

## Next measurements

- Pixdiff of (a)1 against today at the interiors and night spots (expect max <= 2).
- A `PerfTimers.h` scope for the world map pass (today inside `actor-shadows`, logged every 120
  frames only).
- A pan along a fixed yaw sequence, A/B, to separate motion cost from view cost.
