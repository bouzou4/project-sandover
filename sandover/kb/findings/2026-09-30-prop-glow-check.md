---
title: Which prop lights have a source the game draws brighter than its surroundings
date: 2026-09-30
issue: #267
status: verified (measurements); inferred (in-game effect, until the next capture round)
sha: crystal-lights (#301)
---
## Answer (≤5 lines)
The prop list had been chosen by brightness against the whole level, so village3's matte blue
mushrooms lit the area cyan. Measured in vanilla rig frames (`sandover-lights glowcheck`), only the
snow crystals (at the SDR ceiling) and maincave's cave mushrooms are drawn brighter than what
surrounds them; village3's and rolling's mushrooms and lavatube's precursor light are not. The bake
now keeps a prop texture only on that evidence; textures with no measured frame give no light.

## Method
`glowcheck` z-buffers the level background into a texture-id map at the rig projection (80 degrees
horizontal, 16:9, quarter resolution) and, per texture, compares the median sRGB luma of its pixels
with the median of the other textures' pixels whose surface lies within 5 m; Jak and the actors
within 25 m of him are masked with boxes. It also reports the share of pixels with a channel at 250
or more (the SDR ceiling). Projection checked by overlay on probe-8 village3 and maincave frames
(masks land on the mushrooms). Play-run frames (3840x2054, `runs/*/vanilla`) do not line up with
this projection and are not used.

Rule (`props.glows_in_vanilla`, medians over the frames): contrast >= 1.3 and saturation of its
mean colour ((max - min) / max) >= 0.6, or ceiling share >= 0.5 and at least twice the
surroundings'. Luma contrast alone does not separate the classes once actors are masked: the cave
mushroom measures 1.34-1.65, village3's mushroom 1.46-1.55. What does: the cave mushrooms are vivid
(saturation 0.78-0.92), the village3 and rolling mushrooms pale (0.45-0.49), brighter by albedo.
The thresholds sit in the gap of these six classes; more frames may move them.

## Prop textures (after the actor-mask fix; contrast / saturation / ceiling own-around)
| Texture | Frames ($SANDOVER_DATA) | Measured | Light |
|---|---|---|---|
| `snow-crystals` | probe-8-vanilla snow-crystal-crate; crystals-004/C 0019 | 1.15 / 0.70 / 0.96-0.37; 1.75 / 0.68 / 0.93-0.14 | yes (ceiling) |
| `cv-tallshroom-decal-01` | probe-8-vanilla tallshroom; crystals-004/C mushroom, bluecrystal | 1.55, 3.07, 2.15 / 0.85-0.92 | yes |
| `cv-mushroom-01-decal-01` | probe-8-vanilla tallshroom; crystals-004/C mushroom; eco-reference dark-crystal | 1.43, 1.65, 1.34 / 0.78-0.88 | yes |
| `vil3-mushroom-01` | probe-8-vanilla bluemushroom; crystals-004/C vil3-mushroom | 1.55, 1.46 / 0.45, 0.49 | no (pale) |
| `vil3-mushroom-02` | the same two | 0.64, 0.51 | no |
| `ya-mushroom-whole-01` | crystals-004/C rolling (22 h) | 1.46 / 0.46 | no (pale) |
| `lava-precursor-bluelight` | goldens auto-v1 lavatube-start-noon | 1.01 / 0.30 | no |
| `cv-bluecrystal`, `ogr-mushroom-whole-01`, `vil3-giant-crystal-nt-01/03`, both `jngb-*-glow`, `cit-pinkbeam`, `lava-vent-fire` | none in view in any rig vanilla frame | - | no, until measured |

Example colours (mean sRGB, own vs surroundings): village3 mushroom (35, 60, 67) vs (28, 42, 49);
cave tall mushroom (24, 95, 72) vs (14, 60, 46); snow crystal (77, 220, 254) vs (74, 186, 224).
For comparison, the static `vil3-crystal-01` (already lit) measures 4.72 with 72% of its pixels at
the ceiling.

## Not covered by this rule: static emitters
The static producer uses the emitter score (baked E), which this branch does not change. The same
contrast measure does not classify statics on its own: in `goldens/.../firecanyon-end-noon` (and
night) `fcn-bridge` measures 0.88 (0.93), which the rule would drop, but
`fcn-precursor-metal-edging-01` 2.45 (bright metal) and `fcn-painstripe-ground` 2.61 (paint) would
pass, while the lava wall base `fcn-rockwall-bttm` measures 1.31 (1.07): lava reads by hue, not
luma. Statics need an albedo-independent signal (baked E share, or a hot-texel chroma test for
lava); maincave's emitter-scored `cv-wall-*` textures also measure 0.6-1.1.

## Dark eco crystal levels (goal_src/jak1/levels/maincave/dark-crystal.gc)
- Idle: `color-mult` 0.5, `color-emissive` 0 (init, lines 528-549). No light.
- Countdown (`dark-crystal-activate`, 413-428): 9 flashes, each `color-mult` 1 and `color-emissive`
  (1, 1, 1) for 0.1 s, then unlit for 180, 150, 120, 90, 60, 30, 15, 7, 3 ticks (0.6 s down to
  0.01 s, `*dark-crystal-flash-delays*`, line 43).
- Explode (430-467): the model is hidden a frame later; `group-dark-crystal-gnd-explode` fires
  part 2154, a 24 m purple starflash (r 192-256, g 0-32, b 128-192) at alpha 128 fading to 0
  over its 0.18 s timer.
Gate 15 now returns the model's own emissive (1 = present but dark) and, while exploding, the
starflash's linear fade.

## The hard dark wedge beside the dark crystal (probe-8 maincave-darkcrystal-flash-crate)
B - A shows no added light in a hard-edged region next to the crystal, ending in a straight edge on
the lit floor. The chest `crate-3015` stands 2.8 m from `dark-crystal-3`, hidden behind the
crystal from that camera; a box of its size, ray-cast from the light, shadows the region behind
the crystal and reaches the wedge. The crystal itself does not cast (its model is in
`Merc2::is_light_emitter`), the wrapped N.L of the floor faces there is above 0.6, and outside the
lamp map the lookup counts as lit. Inferred: the wedge is the chest's shadow from the crystal's
light, physically right. Proof is the new side shot, which has the chest in frame.
