---
title: Dark trail behind Jak on snow and on the sunken ramp (AO history ghosting)
date: 2026-10-23
issue: ""
status: fixed on branch fix/temporal-ghosting
sha: branch fix/temporal-ghosting (on master 6d6ef3974)
---
## Answer (5 lines)
The trail is the GTAO history (`TemporalAccum`), not the upscaler or motion blur. Ground that Jak
darkened with ambient occlusion keeps that darkening in the AO history after he leaves: the
history texel still sits on the same surface, so the depth test keeps it, and with 0.9 of the
history kept per frame the occlusion fades over 20-30 frames, one imprint per frame. Fix: clip
the history to this frame's AO around the texel (3x3 mean +- 1.5 std devs, inside the min/max,
widened to this frame's own value).

## Reports
Play run 20261023-1042, entries 0014 (sunkenb, noon: "Awful ghosting while sliding down the ramp
here") and 0019 (snow, noon: "Terrible ghosting on the snow from Jak"). Player settings: FSR 4 at
67 %, motion blur Strong, camera blur Vehicles, AO Ultra, shadows high.

## Which stage
Scripted runs on the play build (6d6ef3974): Jak at the bundle's position, the follow camera's
pose frozen, the pad held for 28 fixed-step frames, a 12-frame screenshot burst, then the same
camera with Jak hidden (the plate). One stage toggled per arm, everything else as the player had it.

| Arm (snow, same run) | trail |
|---|---|
| player settings (FSR 4, blur Strong, AO history) | yes |
| FSR 3.1 instead of FSR 4 | yes |
| motion blur off | yes |
| world and actor sun-map lookups off (shadow probe 6) | yes |
| MSAA (no upscaler), blur and AO history on | yes |
| AO history off (`pc-set-ssao-param 24 0`) | no |
| MSAA, blur off, AO off | no |

The trail follows the AO history and nothing else. It reads as a comb: one darker copy of Jak's
contact occlusion per frame, each 10 % fainter than the next.

## Mechanism
GTAO runs at half resolution with a noise pattern that rotates every frame, and `TemporalAccum`
averages it: each texel reprojects to last frame (camera reprojection, or the merc velocity on a
character's own surface), accepts the history while the stored depth matches, and keeps 90 % of
it. That rejects disocclusion (a surface that was hidden) but not a change in lighting on a
surface that stayed put. When Jak runs over snow, the snow under and behind him was occluded by
his body; once he has moved on, the snow is the same surface at the same depth, so the history
passes the depth test and its darkening decays at 0.9 per frame: after 10 frames 35 % is left,
after 20 frames 12 %. At running speed that is a trail several Jaks long. The upscaler cannot
remove it: the darkening is already in its input colour, on pixels whose motion vectors are
(correctly) static ground.

Fix (`temporal_accum.frag`): before the blend, the reprojected history is clipped to this frame's
signal around the texel: the 3x3 neighbourhood's mean +- 1.5 standard deviations, intersected
with its min/max, then widened to include this frame's own value (otherwise an isolated dark
texel, a grass blade or a wire, would be clipped brighter than both history and frame and flicker
with the noise). In the trail this frame's AO says "open snow" all around, so the stale
darkening is cut to the open value at once. On a static surface the converged history sits near
the neighbourhood mean, inside the box, so convergence is unchanged. The plain min/max box was
tried first: it removed the comb on snow but left a darker patch on the sunken floor, whose
grooves make the 3x3 range wide (see results).

Dev switches: `(pc-set-ssao-param 25 0)` drops the clip, `(pc-set-ssao-param 26 <x 1e6>)` sets its
width in std devs (0 = min/max). AO off and AO history off are unchanged.

## Results
Test: the mean wake darkening over burst frames 6-11 (luma levels) minus the AO-history-off arm
of the same run (the no-trail floor); a trail fails at an excess above 4 levels. Scenes: snow at
the bundle's Jak position, the follow camera's pose frozen, Jak running across it; sunkenb at the
bundle's Jak position, camera 12.6 m straight above him (the follow camera's pose there), Jak
running up the ramp. Player settings in every arm (FSR 4 at 67 %, motion blur Strong).

| Run | clip off (or play build) | clip on (shipped) | history off (floor) | excess off -> on |
|---|---|---|---|---|
| snow, play build 6d6ef3974 (`base-snow4`) | 9.85 | | 1.72 | 8.1 fail |
| snow, branch (`final-snow`) | 9.30 | 3.37 | 1.57 | 7.7 fail -> 1.8 pass |
| sunkenb, branch (`final-sunk3`) | 11.62 | 2.03 | -0.58 | 12.2 fail -> 2.6 pass |
| sunkenb, branch merged with master 3909d4c9b (`merged-sunk`) | 10.08 | 2.70 | -0.27 | 10.4 fail -> 3.0 pass |
| sunkenb, clip widened to this frame's value (`rev-sunk`) | 11.32 | 1.83 | 1.32 | 10.0 fail -> 0.5 pass |
| snow, clip widened to this frame's value (`rev-snow`, footprint from the history-off arm) | 7.10 | 1.33 | -0.10 | 7.2 fail -> 1.4 pass |

(snow: wake 80 px from Jak inside the trail box; sunkenb: 120 px, Jak's blurred legs sit closer.)
A second framing of snow (`g-snow`, a bigger trail) gave clip off 33.95, min/max 0.28, 1.5 std
devs 0.11, history off -1.56. The plain min/max box removed the snow trail (`proof-snow`) but
left a darker patch on the sunken floor, whose grooves make the 3x3 range wide; the 1.5 std dev
clip removes it. Static stability (frame-to-frame |d| of the Jak-hidden plates, `g-snow`): clip
off 0.317, min/max 0.386, 1.5 std devs 0.251, history off 0.554: the clip does not add shimmer.

## Cost
GPU timers over 120-frame windows, clip off/on/off/on in one boot at a static camera (Jak
standing), 2560x1440 render (67 % of 4K), AO at half resolution, shared GPU load (not a solo run):
the AO history + denoise scope (`ssao-blur`) 0.283 -> 0.324 ms (snow), 0.327 -> 0.360 ms (sunkenb
bundle area): +0.03-0.04 ms, nine extra half-resolution texel reads. Frame time unchanged at the
60 Hz cap (16.67 ms in both states).

## Data
`$SANDOVER_DATA/shots/temporal-ghosting/`: `base-snow4` (stage toggles, play build), `proof-snow`,
`proof-sunk` (min/max clip, in-run A/B), `g-snow` (clip variants), `final-snow`, `final-sunk3`,
`merged-sunk` (clip A/B and cost), `rev-sunk`, `rev-snow` (shipped clip). Metric: the wake is where Jak was earlier in the burst (from the
no-temporal arm against its plate) at least 80 px from where he is now; the ghost is the mean
darkening of the wake against the same arm's plate (luma 0-255).

## Follow-up
Re-shooting at the bug report's exact camera did not match the screenshot. Per review, the
from-snapshot camera placement is correct; the differences come from the report's FOV being
dropped (95 on the sunken ramp, 64 on snow), the continue-point swap on sunkenb and the static
cam-fixed shot. Carrying the FOV into the replay is the follow-up.
