---
title: Eco brightness - what draws orbs, vents and power cells, and a rule for their emissive step
date: 2026-09-24
status: verified
sha: branch eco-brightness (base e28b3771f)
---
## Answer (≤5 lines)
None of the eco elements are background geometry, so the sidecar never gives them E. Vents,
eco clusters, power cell flares and eco beams are additive sparticles (Sprite3), which already
write `0.4 x brightest channel` each; stacked, that reaches full E and blows out under bloom.
Precursor orbs and the power cell body are merc models and wrote no E. An actor emissive table
(override rows plus a ladder step per texture) now drives both renderers, with a step chosen so
the element's baked peak stays under 0.98 after bloom.

## What draws each element (Jak 1)

| Element | Renderer | Textures (combo_id) | Baked brightness | E before |
|---|---|---|---|---|
| Blue eco cluster (`eco-blue`, `group-eco-blue`) | Sprite3, 2D particles | `bigpuff` 0x00020000, `hotdot` 0x0002000f | additive (`bit3`), low vertex colour | 0.4 x channel, under the ramp at night |
| Blue eco vent (`ventblue`/`ecovent`, `group-part-vent-blue-active`) | Sprite3, additive (`bit3`) | `starflash` 0x00020012, `middot` 0x00020004, `lightning*` 0x0002001f/23/24 | additive stacking of 10+ sprites | 0.4 x channel per sprite, summed: saturates |
| Power cell (`fuel-cell`) | merc body (`fuel-cell-inside` 0x01cf0007, endcaps, precursor metal) + Sprite3 starburst (`flare` 0x00020002, `starflash`) | merc vertex colour x2 (`merc2.frag`), additive flares | merc: none; flares: summed |
| Precursor orb (`money`) | merc (`egg-ndimadman` 0x01cf002c) + one glint sprite (`hud-egg` 0x0002002c) | merc x2, glint additive | merc none, glint small |
| Eco platform arcs (`plat-eco`) | Sprite3 `lightning*`, merc `cmn-precursor-blue-glow` | additive | summed |
| Mirror beam, sage beams (`draw-eco-beam`) | Sprite3 3D `citadel-shield` 0x0002000c | additive | summed |
| Torches, fire (e.g. `group-part-citadel-torch`) | Sprite3 `bigpuff`, additive, red/orange vertex colour | additive | summed |

Sources: `goal_src/jak1/engine/common-obs/collectables.gc` (`eco-collectable`, `money`, `fuel-cell`,
`vent`), `collectables-part.gc` (part groups 42, 44, 63, 64), the flag-to-blend mapping in
`game/mips2c/jak1_functions/sparticle_launcher.cpp` (`sp_launch_particles_var`), (part flag `bit3` selects the additive `gs-alpha`, `#x48`), and the model
glbs' material names. No laser actors exist in Jak 1; the beams are the sprites above.

## The brightness rule (calibration input)

Bloom adds about `strength x w x (c - threshold)` to a pixel of baked colour `c` inside a large
element, with `w = smoothstep(0.1, 0.4, E)`. Under the clamp tonemap an element whose baked peak
is `p` stays bright but unclipped when `p + w (p - 0.3) <= 0.98`, so

    w_max = (0.98 - p) / (p - 0.3)      (0 if p >= 0.98, 1 if p <= 0.3)

and the ladder step is the highest quarter octave whose E gives at most `w_max`
(`sandover.materials.actor_emissive.ladder_step`). `p` is measured at 23:00 on vanilla
(`sandover/shots/eco-emitters.yaml`); at noon most of these elements already clip in vanilla
(vent 51%, cell 47%, eco cluster 44% of their box at 1.0), so the rule is set by night, where
the headroom exists, and adds nothing at noon beyond what vanilla clips.

| Texture | Element | Peak p (night) | w max | Ladder step | E |
|---|---|---|---|---|---|
| bigpuff, hotdot | eco cluster, fire | 0.61 | 1.0 | 0.354 | 0.35 |
| citadel-shield | beams | 0.53 | 1.0 | 0.354 | 0.35 |
| hud-egg, egg-ndimadman | orb | 0.71 | 0.66 | 0.25 | 0.25 |
| lightning 1-3 | arcs | 0.94 | 0.06 | 0.125 | 0.12 |
| starflash, flare, middot, fuel-cell-inside | vent, cell | 0.97 | 0.015 | 0.105 | 0.10 |

Shared particle textures carry a `min_chroma` gate: a listed sprite writes E only where its
vertex colour is saturated (eco blue, fire orange), so white dust and smoke on `bigpuff` stay out.
The table writes E with a max blend, so stacked vent particles no longer sum past the ramp.

HDR: under `pc-set-hdr-target` bloom's copies are now RGBA16F, so a vent's baked value above 1
reaches the bright pass and the composite stays above 1 until the tonemap. For HDR output the
same rule applies with `cap` raised to the display's peak over paper white; the peaks and steps
above are the inputs, nothing else changes.

## Evidence

Captures (2560x1440, MSAA 4, particles on, Jak hidden): `$SANDOVER_DATA/shots/eco-emitters/{vanilla,
before,after,brighter,fp16,eview}/`; the `eco-001` capture set. `before` =
bloom with an empty table (the current behaviour), `after` = bloom with the table, `brighter` =
table E doubled. Brightest-channel stats inside a box around each element (share of pixels at
1.0 / 90th percentile of the unclipped top fifth):

| Element, 23:00 | vanilla | before | after | brighter |
|---|---|---|---|---|
| jungle vent | 30.5% | 36.3% | 30.3% | (shot failed to stage) |
| temple-top power cell | 10.2% / 0.97 | 20.5% / 1.00 | 17.8% / 0.98 | 17.7% / 0.98 |
| eco platform | 13.1% / 0.96 | 18.5% / 0.98 | 11.2% / 0.93 | 20.1% / 1.00 |
| village1 eco cluster | 0.5% / 0.62 | 5.2% / 0.79 | 1.7% / 0.70 | 4.6% / 0.74 |
| precursor orb | 0.0% / 0.71 | 0.0% / 0.71 (no E) | 0.6% / 0.84 | 2.8% / 0.92 |

So: orbs now glow (0.71 to 0.84, under the cap); the vent and eco platform are back to vanilla's
clipping instead of above it; the cell still clips more than vanilla (its flares are the brightest
additive stack and the merc core adds a halo); doubling the steps reintroduces clipping.

Parity (toggles off): `sandover-parity check --set auto-v0 --build p2eco-1`, golden f953354f0,
threshold 0.011399. Two runs (the rig was shared with other captures and each lost shots to
start-up crashes) together cover 15 of 16 shots, all passing: worst FLIP mean 0.0016
(sunken-start-noon), village1 and beach at or under 0.0003, most at 0. village1-hut-noon was not
captured in either run. The table is read only while the emissive mask is live, the merc and sprite shaders
write location 2 only when that draw buffer is routed, and bloom's float copies only exist with
the FP16 target on.

## Implications for the design
- Emissive for actors and particles is keyed by texture, not by the bake: the reviewer's `E` on
  a merc texture now reaches the engine through `sandover-materials actor-emissive`.
- Baked-bright content needs a per-element headroom term in the rule above, with `cap` the
  SDR 0.98 or the HDR peak.

## Follow-ups
- The power cell still exceeds vanilla clipping: its `flare`/`starflash` step is at the ladder
  floor's neighbour; a per-element bloom weight (not per texture) would be needed to go lower.
- Envmapped merc (emerc) and generic draws do not write the table's E.
- Box statistics are hand-placed; the vent brighter shot failed to stage (camera glitch) and was
  not rerun.
