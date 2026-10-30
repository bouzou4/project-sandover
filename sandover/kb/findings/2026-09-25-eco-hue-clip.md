---
title: Eco colour at D - hue-preserving bloom clip, and why the vent escaped the brightness rule
date: 2026-09-25
status: verified
sha: branch eco-hue (base 83156a039)
---
## Answer (≤5 lines)
D (rule + 1 octave) is now the seeded default (`emissive.toml` `octaves = 1`). The vent went white
because bloom was added per channel: once blue hit 1.0, red and green kept rising. The bloom
composite now scales the whole pixel by `1 / max(r, g, b)` of `min(frame, 1) + bloom` when that max
exceeds 1 (`pc-set-bloom 4`, on by default). The vent's rule input was also wrong: its peak came
from unclipped night pixels, but the vent clips in vanilla (30% of its box at night, 63% at noon).

## Why the vent escaped the rule
- The rule caps the brightest channel only: `p + w (p - 0.3) <= 0.98`. Its premise at noon ("adds
  nothing beyond what vanilla clips") holds for the brightest channel only. With a per-channel clip,
  bloom still raises the other channels of a pixel that has already clipped, until they reach 1 too.
- The vent's peak (0.97) was the 90th percentile of the *unclipped* top fifth at 23:00. The vent's
  additive sprites stack past 1.0 before bloom (30% of the box at night). At noon they also sit on
  sunlit sand (63% of the box at 1.0 in vanilla, mean clipped colour 0.94/0.87/0.92). So the true
  peak is 1.0, and no step above the ramp's foot is clip-safe. The rule's 0.105 step was really
  "at the foot of the ramp" (w ≈ 0). D doubles it to E 0.21 (w ≈ 0.31), and that clipped.
- Fix of the input: `middot`, `starflash`, `flare` and `fuel-cell-inside` now have `peak = 1.0` and
  an explicit `step = 0.105`. That is the value seen at D/2, so D's numbers do not change.
  With the hue clip, the rule's noon premise holds: a clipped pixel keeps its brightest channel at 1,
  and the others scale down with it rather than rising.

## Clip
`bloom_add.frag` returns `(b / m, 1 / m)` with `m = max(1, max_c(min(frame, 1) + b))`, blended
`GL_ONE, GL_SRC_ALPHA`, so each MSAA sample becomes `(dst + b) / m`. The frame is the resolve bloom
already takes. Where `b = 0` or `m = 1`, the result is the old additive result exactly. Under FP16,
the scale uses the frame clamped to 1. The clamp tonemap then matches `RGBA8` wherever the frame is
at or under 1. Filmic shapes the scaled pixel. Bloom off: no code runs, so the off path is unchanged.

## Evidence
Captures (2560x1440, MSAA 4, particles on, D table): `eco-002`
(A current D with `pc-set-bloom 4 0`, B with the hue clip; table `emissive-actors-D.csv`). Vent box
(same box as the `eco-001` capture set), share at max channel 1.0 / all channels at 0.97 or above / mean saturation of
the clipped pixels:

| jungle-vent-noon | clipped | white | saturation |
|---|---|---|---|
| vanilla (eco-001 A) | 62.9% | 27.4% | 0.19 |
| D (eco-001) | 68.7% | 44.9% | 0.12 |
| current D (eco-002 A) | 67.3% | 39.7% | 0.14 |
| D + hue clip (eco-002 B) | 66.1% | 30.7% | 0.19 |

jungle-top-cell-night, whole frame: current D is 0.6% white, with 0.34 saturation on its clipped
pixels. With the hue clip it is 0.0% white and 0.53 (vanilla: 0.0%, 0.47). At noon the vent's core
is still white, as in vanilla: the sprites clip in the 8-bit framebuffer before bloom runs.
Keeping that colour needs the FP16 target.

Cost: one extra texel fetch per pixel in the composite; not measured.

## Follow-ups
- The main data has no `emissive-actors.csv`. Run `sandover-materials actor-emissive --seed` to put
  D live outside rig runs.
- Under FP16 with the clip, the vent core's pre-bloom overbright would keep its hue. That was not
  captured.
