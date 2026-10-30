---
title: Frame profile re-baseline after the 2026-10-23 optimisation merges (RDNA 4, 4K)
date: 2026-10-23
issue: ""
status: measurement only (no engine change)
sha: master 376d4cb19
---
## Answer (≤5 lines)
Config A (everything maxed, FSR 3.1 at 100 %) went from a 33.3 ms mean to 20.3 (median 28.9 ->
18.4) and from 2 to 7 of 21 spots at 4K60. Config B (FSR 4 at 67 %) went from 21.7 to 16.8 (median
18.5 -> 16.4) and from 9 to 11 of 21. Both still miss 4K60: the worst spots are now 30.5 ms in A
(beach-shore, jungle) and 23.8 in B (village2-night-lamps). The frame no longer has one dominant
cost: motion blur, GTAO, the background renderers and the post chain each take 2-6 ms.

## 1. Setup
- The same rig and recipe as `2026-10-22-perf-rdna4.md`: `sandover-perf`, `shots/perf.yaml`
  (21 spots), 3840x2160, vsync and limiter off, `-nosound`, 120-frame windows for the
  configurations, one instance, and a gate before each boot (no gk, GPU 3D engine under 10 %,
  exponential backoff).
- Build: master `376d4cb19` (gk + goalc, Release, `-j` = logical threads - 2 under the build
  lock), staged under `$SANDOVER_DATA/bin/perf-rebaseline`. The project root is staged per
  `rig-capture.md` ("Worktree builds") at `$SANDOVER_DATA/worktrees/perf-rebaseline-root`, with
  `third-party/fidelityfx/` and a compiled `out/jak1` whose `goal_src` matches master
  (`outputs_fresh()` true, so the types-only `(mi)` was enough). gk logged
  `fsr3: targets 3840x2160 -> 3840x2160` and `fsr4: available` in every boot.
- After-load forms: the same as on 10-22, except AO quality. A: `sandover-settings-reset!`,
  contact shadows on, motion blur strong (3), **AO quality ultra (3)**, shadow quality high (2),
  upscaler FSR 3.1 (0), render scale 100 %, `sandover-settings-reapply`. B: A with FSR 4 (2) at
  67 % (2). C: `vanilla?` #t.
  On 10-22, AO quality 2 was the top tier of the old SAO kernel. GTAO came after that
  build and added the ultra tier (3 x 3, full resolution), so A and B now carry GTAO ultra: see 4
  for what that costs.
- A sampler on the GPU driver's power-management log recorded the GL card's clock, activity and temperature and
  the gk count every 2 s (`adl.csv`, `adl-r2.csv`).
- Data: `$SANDOVER_DATA/perf/rebaseline-1023/` (`probe`, `A` (discarded, see 6), `A-r2`, `B`,
  `C`, `G` for the GTAO delta; `gk.out.log` and `adapter.txt` per run).

## 2. Before / after per spot (GPU frame ms, mean of the 120-frame base window)
`x` = misses 16.7 ms. "max" = the worst single frame in today's window. Top groups exclude
`present` (HDR encode and flip, 1.4-2.8 ms at every spot).

### A: everything maxed, FSR 3.1 at 100 % (run A-r2)

| spot | 10-22 | now | saved | max | top groups now |
|---|---|---|---|---|---|
| beach-shore | 48.1 x | 30.5 x | 17.6 | 36 | ocean-near 5.1, motion-blur 3.3, ssao 3.3 |
| open-sea-low | 50.3 x | 16.5 | 33.8 * | 18 | vista 2.7, motion-blur 2.4, ocean-mid 2.1 |
| open-sea-high | 17.2 x | 17.1 x | 0.0 | 19 | vista 3.3, motion-blur 2.9, fsr3 1.8 |
| village1-night | 50.9 x | 23.2 x | 27.7 | 26 | motion-blur 2.6, tfrag 2.4, tie 2.4 |
| hut-roof | 29.8 x | 18.4 x | 11.3 | 20 | vista 2.9, motion-blur 2.7, fsr3 1.7 |
| jungle | 44.5 x | 30.5 x | 13.9 | 33 | tie 6.1, ssao 3.4, motion-blur 3.3 |
| temple-top | 22.8 x | 24.0 x | -1.3 | 26 | motion-blur 4.3, fsr3 2.4, tie 2.0 |
| low-behind-jak | 37.9 x | 24.2 x | 13.7 | 26 | tie 2.8, motion-blur 2.7, ssao 1.8 |
| sunken | 28.9 x | 19.2 x | 9.7 | 22 | tfrag 3.4, ssao 2.7, motion-blur 2.6 |
| maincave | 20.3 x | 16.6 | 3.7 | 18 | ssao 3.0, tfrag 2.7, motion-blur 2.5 |
| darkcave | 20.1 x | 15.9 | 4.2 | 17 | tfrag 3.0, ssao 2.9, motion-blur 2.6 |
| snow | 26.1 x | 17.5 x | 8.6 | 20 | motion-blur 2.6, ssao 2.5, tfrag 2.4 |
| swamp | 32.3 x | 20.3 x | 12.0 | 22 | tie 3.0, ssao 2.9, motion-blur 2.4 |
| swamp-dock1 | 46.2 x | 24.9 x | 21.3 * | 27 | tie 5.8, ssao 2.6, motion-blur 2.5 |
| village1-hut-lamps | 80.5 x | 23.9 x | 56.6 * | 26 | tfrag 4.5, ssao 3.4, motion-blur 2.7 |
| village2-night-lamps | 62.9 x | 29.3 x | 33.6 * | 31 | tie 5.1, tfrag 3.1, ssao 3.0 |
| maincave-crystals | 12.4 | 14.2 | -1.8 | 16 | ssao 2.9, motion-blur 2.5, tfrag 1.7 |
| darkcave-0024 | 17.5 x | 15.9 | 1.6 | 17 | tfrag 3.4, motion-blur 2.5, ssao 2.5 |
| darkcave-0025 | 17.8 x | 16.0 | 1.7 | 17 | tfrag 3.5, motion-blur 2.7, ssao 2.4 |
| ogre | 23.7 x | 17.9 x | 5.8 | 19 | tie 3.4, motion-blur 2.6, ssao 2.4 |
| finalboss | 9.6 | 10.2 | -0.7 | 11 | motion-blur 2.5, fsr3 1.4, ssao 1.4 |

Mean 33.3 -> 20.3 ms, median 28.9 -> 18.4, 2 -> 7 of 21 at 4K60.
`*` These spots were in a slow 10-22 boot (A2: `fsr3` 3.1-5.0 ms against 1.4-2.3 elsewhere).
Against the fastest 10-22 boot of the same configuration (`2026-10-22-perf-rdna4.md` 2), the
savings are open-sea-low 23.2 -> 16.5 (6.7), swamp-dock1 43.1 -> 24.9 (18.2) and
village2-night-lamps 50.9 -> 29.3 (21.6). The spots that got slower (temple-top,
maincave-crystals, finalboss) are light on tie and tfrag: there the GTAO ultra cost (4) outweighs
the background savings.

### B: A with FSR 4 at 67 % (run B)

| spot | 10-22 | now | saved | max | top groups now |
|---|---|---|---|---|---|
| beach-shore | 25.9 x | 21.5 x | 4.4 | 23 | fsr4 4.0, ocean-near 3.0, tie 1.8 |
| open-sea-low | 17.8 x | 16.4 | 1.4 | 18 | fsr4 3.7, vista 3.1, ocean-mid 2.1 |
| open-sea-high | 12.2 | 15.3 | -3.1 | 18 | fsr4 3.9, vista 3.5, actor-shadows 1.2 |
| village1-night | 30.3 x | 21.1 x | 9.2 | 23 | fsr4 4.1, tie 2.2, vista 2.2 |
| hut-roof | 18.5 x | 16.9 x | 1.5 | 21 | fsr4 3.9, vista 3.0, tie 1.3 |
| jungle | 25.0 x | 23.3 x | 1.7 | 25 | fsr4 4.2, tie 4.2, tfrag 2.1 |
| temple-top | 15.7 | 15.9 | -0.2 | 18 | fsr4 3.9, vista 1.3, tie 1.2 |
| low-behind-jak | 23.4 x | 22.0 x | 1.4 | 24 | fsr4 4.0, tie 2.6, vista 1.6 |
| sunken | 28.6 x | 17.6 x | 11.0 | 19 | fsr4 4.1, tfrag 2.9, ssao 1.8 |
| maincave | 14.6 | 15.0 | -0.4 | 17 | fsr4 4.0, tfrag 2.4, ssao 2.0 |
| darkcave | 36.1 x | 11.2 | 24.9 | 12 | fsr4 3.4, tfrag 1.7, ssao 1.3 |
| snow | 15.6 | 15.4 | 0.2 | 17 | fsr4 3.8, actor-shadows 2.0, tfrag 1.9 |
| swamp | 20.2 x | 18.4 x | 1.8 | 20 | fsr4 3.9, tie 2.6, ssao 1.9 |
| swamp-dock1 | 26.3 x | 22.6 x | 3.7 | 24 | tie 5.1, fsr4 4.0, tfrag 2.2 |
| village1-hut-lamps | 47.1 x | 20.1 x | 27.0 | 22 | fsr4 4.0, tfrag 3.3, ssao 2.1 |
| village2-night-lamps | 40.9 x | 23.8 x | 17.1 | 25 | fsr4 4.2, tie 3.6, tfrag 2.2 |
| maincave-crystals | 9.5 | 9.9 | -0.5 | 11 | fsr4 3.5, ssao 1.2, tfrag 0.9 |
| darkcave-0024 | 12.2 | 11.9 | 0.3 | 14 | fsr4 3.0, tfrag 2.3, ssao 1.3 |
| darkcave-0025 | 12.4 | 13.6 | -1.2 | 15 | fsr4 3.3, tfrag 2.8, ssao 1.4 |
| ogre | 14.9 | 12.0 | 2.9 | 13 | fsr4 3.4, tie 1.8, tfrag 1.1 |
| finalboss | 9.0 | 9.7 | -0.7 | 11 | fsr4 3.8, motion-blur 1.0, ssao 1.0 |

Mean 21.7 -> 16.8 ms, median 18.5 -> 16.4, 9 -> 11 of 21 at 4K60. The 10-22 anomalies are gone:
`actor-shadows` reads 1.1 ms mean (was 7.9-15.0 at sunken and the lamp spots), and darkcave's
`fsr4` span is 3.4 ms (was 14.6). open-sea-high is 3.1 ms slower, mostly in `vista` (3.5 against
2.7) and `actor-shadows` (1.2); it is a single-boot reading.

### C: vanilla (run C)
GPU work without the present wait is 0.6-2.1 ms at every spot, 0.0-0.2 ms under 10-22 at each
spot. Vanilla rendering is unchanged, as expected.

## 3. 4K60 verdict
- **A fails**: 7 of 21 meet 16.7 ms (finalboss 10.2, maincave-crystals 14.2, darkcave 15.9,
  darkcave-0024 15.9, darkcave-0025 16.0, open-sea-low 16.5, maincave 16.6; the last four with
  worst frames of 17-18 ms). Misses by under 2 ms: open-sea-high 17.1, snow 17.5, ogre 17.9.
  Misses by more: hut-roof 18.4, sunken 19.2, swamp 20.3, village1-night 23.2, village1-hut-lamps
  23.9, temple-top 24.0, low-behind-jak 24.2, swamp-dock1 24.9, village2-night-lamps 29.3, jungle
  30.5, beach-shore 30.5.
- **B fails**: 11 of 21 meet (finalboss, maincave-crystals, darkcave, darkcave-0024, ogre,
  darkcave-0025, maincave, open-sea-high, snow, temple-top, open-sea-low). Misses: hut-roof 16.9
  (0.2 over), sunken 17.6, swamp 18.4, village1-hut-lamps 20.1, village1-night 21.1, beach-shore
  21.5, low-behind-jak 22.0, swamp-dock1 22.6, jungle 23.3, village2-night-lamps 23.8.
- C: 0.6-2.1 ms of GPU work.

## 4. GTAO delta (ultra -> off, config A, run G)
`--toggles ssao` (AO off, tile lights left on; on master they keep their prerequisites without
AO), 60-frame windows, A/B/A in one boot, solo:

| spot | frame (base) | AO off saves (frame) | `ssao` group | `ssao-ao` | noise band |
|---|---|---|---|---|---|
| beach-shore | 29.5 | 0.26 | 3.06 | 2.26 | 0.15 |
| jungle | 29.5-30.0 | 0.70 | 3.23 | 2.36 | 0.95 |
| village1-hut-lamps | 24.2-24.4 | 2.74 | 3.43 | 2.60 | 0.33 |

The pass costs 3.1-3.4 ms of GPU time at 4K ultra, mostly the AO kernel (`ssao-ao`). The frame
saves less than that at beach-shore and jungle: with AO off, `ocean-near` (beach-shore, +0.7) and
`tie` (jungle, +0.8) read slower in the same windows. The cause is not established (the jungle
saving is inside its noise band). Across the 21 spots of A-r2 the `ssao` group averages 2.36 ms
(max 3.41), against 0.83 for the old SAO high tier on 10-22.

## 5. Top remaining costs for the next wave (config A, mean / max over the 21 spots)
1. Motion blur: 2.75 / 4.27 ms (temple-top 4.3). A fixed 4K full-screen pass, at the same cost as
   on 10-22; level-independent (10-22 deltas).
2. Background renderers `tie` + `tfrag`: 1.94 / 6.11 + 2.19 / 4.52 ms (jungle `tie` 6.1,
   village2-night-lamps 5.1, swamp-dock1 5.8; village1-hut-lamps `tfrag` 4.5). Down from 8.74 and
   3.93 mean. The overdraw plan item (`2026-10-23-shadow-cost-wave2.md` 6: 71-88 M fragment runs
   for 8.3 M pixels) is the lever.
3. GTAO ultra (`ssao`): 2.36 / 3.41 ms of GPU time; 0.3-2.7 ms of frame (4).
4. HDR present encode (`present-encode`): 1.88 / 2.73 ms.
5. FSR 3.1: 1.67 / 2.42 ms; in B, the `fsr4` GL span is 3.81 / 4.25 ms, the largest group at
   almost every B spot (the interop hand-over, `2026-10-22-perf-rdna4.md` 5).
6. Vista hi-Z reduction (`vista-part:hiz-build`): 1.12 / 2.85 ms (jungle), paid whether or not
   it culls anything (`2026-10-23-vista-cost-wave2.md` 5).
7. Near and mid ocean: `ocean-near` 5.1 ms at beach-shore (3.0 in B), `ocean-mid` 2.0-2.1 at
   temple-top and open-sea-low. Now the largest single group at beach-shore.
8. Shadows: `actor-shadows` 0.78 / 1.35 (the world map prepass `world-shadows` 0.73 / 1.29 of
   it), contact shadows 0.68 / 1.08.
9. Tile lights (`emissive-lights`): 0.85 / 1.10 ms (was 1.80 / 10.2).
10. The `ssao-marks` pass: 0.34 / 0.61 ms.

## 6. Caveats
- Variance: two boots of config A agree within 0.5 ms at the 11 spots both measured solo (A
  against A-r2: beach-shore 30.1 / 30.5, jungle 30.9 / 30.5, temple-top 23.5 / 24.0, others
  closer). The 2-5x slow-boot state of 10-22 did not recur in the five solo boots of today: `fsr3` read
  1.4-2.4 ms and `present-encode` 1.4-2.8 at every spot. The GFX clock stayed at about 3.0 GHz
  under load, with the memory clock at 2505 MHz and a hot spot up to 89 °C. The 10-22 numbers
  marked `*` came from a slow boot, so their savings overstate the change (2).
- Cross-boot B readings carry the same caveat as on 10-22: compare runs taken in the same boot.
- Adapter: all six boots (probe, A, A-r2, B, C, G) logged `GL context on ... PCI bus 3`, the
  display card, with the window line `(the GL adapter)`. The second card stayed at 0 % activity
  throughout.
- Another gk instance ran from 08:11 to 08:18, during the second half of the first A run (snow to
  finalboss, GPU activity 100 %). `sandover-perf` checks for another gk only at start. Those
  spots read 1.5-2.8x slower in fixed-size passes, so that run is discarded and A is the re-run
  A-r2, solo throughout. The sampler's gk column showed only this session's gk during A-r2, B, C
  and G.
- No device loss: no `Present failed`, `GL_CONTEXT_LOST` or device removal in any gk log.
- AO differs between the dates (SAO high on 10-22, GTAO ultra now), so the A and B savings are
  net of GTAO's extra 1.5 ms mean.
- Config C spent 90 s per spot in `RIG:WAIT-TIMEOUT ... an empty vista loader list` (vanilla has
  no vista loader), which made the run 35 min instead of about 11. The numbers are not affected.
- The engine timers give a mean and a worst frame per window, no percentiles.
