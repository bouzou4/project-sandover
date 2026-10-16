---
title: Can sunken's caustics be made smooth without moving where or how strongly they light?
date: 2026-09-30
issue: none
status: inferred (code read and rip statistics; no capture yet)
sha: 62ef896a1 (base; branch sunken-caustics)
---
## Answer (≤5 lines)
Yes, exactly in the mean. The original's caustics are four phases of one coarse per-vertex
pattern in palette slots 4-7, cross-faded by `update-mood-caustics` (32 frames, weights summing to
1.25). Freezing the four weights at their time mean keeps the original's mean light bit for bit;
the shaders add a zero-mean procedural term scaled by the bake's envelope (the mean of the four
slots). Two looks: B1 at the original's scale and tempo, B2 a fine web.

## Evidence
- Loop: `update-mood-sunken` (`goal_src/jak1/engine/gfx/mood/mood.gc:1002`) adds 1 to `state 1`
  per unpaused call and calls `(update-mood-caustics arg0 4 1)` (`:1012`); `update-mood-caustics`
  (`:605`). Levels: every level-load-info whose `:mood-func` calls `update-mood-caustics`, i.e.
  sunken and sunkenb (`sandover-caustics moods`, written to `CausticMoods.inc`).
- Above 60 fps the original runs faster: `update-time-of-day` runs once per display frame
  (`engine/draw/drawable.gc:868`) and the counter steps once per call, so the loop is 32 rendered
  frames (4.5 Hz at 144 fps). Jak's `dir2` flicker reads `integral-frame-counter`, which counts
  frames too (`drawable.gc:1075`). Code read; not run. The pattern clock advances by the real
  frame time (at most 0.1 s) on each frame where the raw weights moved (the game is not paused),
  so its rate is the same at any frame rate, including below the target; `pc-set-caustics 7 <us>`
  holds it at a value (A/B captures), a negative value lets it run.
- Energy (`sandover-caustics stats`, 100k area samples on caustic-lit triangles, Gouraud envelope,
  base = day slot + room light at its mean; luminance in palette steps, 128 = 1.0):

  | | mean total | mean caustic | swing median | swing p90 | swing / total median, p90 |
  |---|---|---|---|---|---|
  | sunken original | 75.88 | 12.69 | 6.50 | 29.71 | 9.4 %, 31.4 % |
  | sunken B1 smooth | 75.88 | 12.69 | 5.57 | 28.71 | 8.3 %, 28.5 % |
  | sunken B2 web | 75.92 | 12.73 | 6.72 | 26.24 | 10.1 %, 25.2 % |
  | sunkenb original | 76.99 | 16.18 | 2.80 | 23.04 | 5.6 %, 21.4 % |
  | sunkenb B1 smooth | 77.00 | 16.18 | 2.98 | 31.32 | 5.9 %, 25.9 % |
  | sunkenb B2 web | 76.99 | 16.18 | 3.10 | 28.21 | 6.5 %, 22.7 % |

  B1 is zero-mean per point over each loop (two noise fields in quadrature); B2 is zero-mean over
  space (`web_mean`). The +0.04 on sunken B2 is the sampling error of `web_mean`.
- Scale: spatial autocorrelation of the original's per-phase deviation falls to 0.5 at about
  2.5 m (sunken) and 1.5 m (sunkenb); the B1 noise with a 6 m cell falls to 0.5 at about 2.7 m.

- Revision 1 (`sunken-caustics-001`, verifier): one global 6 m cell was coarser than
  the original in sunkenb and swung twice as much on the helix floor. The original's scale
  depends on the geometry: on the rips, its phase deviation (slot - envelope) / envelope
  decorrelates to 0.5 at 3.4 m (sunken tfrag), 4.6 m (sunken tie), 3.3 m (sunkenb tfrag) and
  1.7 m (sunkenb tie, the helix wall and glass). Revision 2 takes the cell per level and kind from
  that (CausticMoods.inc) and the swing per palette entry from the four slots (envelope alpha).
  Offline (300k area samples): mean light within 0.1 %; median swing / total 7.9 % vs 9.3 %
  (sunken), 5.0 % vs 5.6 % (sunkenb); p90 35.9 % vs 31.4 %, 22.7 % vs 21.5 %.
- Deadly water: the synced `sunken-water` actors (sunken 6 s / 8 s periods, deadly below phase
  0.5 / 0.3 / 0.25; sunkenb 5 s, offset 0.5, below 0.5; `sunken-water.gc`, `sync-info.gc`) follow
  the game clock, and the rig's epoch differs per shot, so revision 1 caught some shots electrified.
  Every one of them is safe for game clock 13700..14150 mod 36000 ticks.
- The helix continue camera came out 1.26 degrees off in every arm although the rig re-fixes it
  (hypothesis, not proven: a camera region around Jak at the helix continue keeps steering it);
  revision 2 uses explicit cameras away from the helix.
- Revision 2 (sunken-caustics-002) was unusable: shovers (sunken) and bullies (sunkenb)
  filled both frames, and on caustic-lit floor and wall away from them B1 and B2 changed A by a
  mean 1-15 levels (p90 under 30; the sunkenb floor 3-6). Scaling the term by the original's
  relative swing (median 0.3) kept it at parity with a swing the maintainer never noticed. Revision 3 scales
  it by the caustic light itself, masked where the original does not animate (relative swing
  under 0.1), and both looks are the web; amp is how much of that light the web gathers into its
  lines (B1 half, B2 all). Offline: median swing / total 25 % and 50 % (sunken), 16 % and 33 %
  (sunkenb). Cameras come from captured frames: two with no shover or bully within 45 m, the
  sunkenb one turned away from the two bullies 23 and 30 m off (they drifted into revision 2's frame).

## Implications for the design
- The envelope keeps the bake's tile stars (they are in the mean light too, not in the phases).
- Shaders: tfrag3 (and tfrag3_sun), shrub, etie_base; wind TIE and Hfrag have no term (frozen
  mean only). Needs PBR on, like the swamp look.

## Follow-ups
- Actors: Jak and creatures keep the level-wide flicker. Tying them to the local pattern means
  evaluating the same field in merc2.frag for the `dir2` term, gated by an envelope that actors do
  not have: a coarse 3D grid of the envelope baked from the rips, or the background's term read
  from screen space under the actor.
