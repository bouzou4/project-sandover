---
title: Actor shadow spike - sun and lamp maps cast by actors, consistent with the bake
date: 2026-09-23
issue: shadow proposal in kb/design-inputs.md
status: spike works; look awaits the maintainer
---
## Answer (<=5 lines)
Actors cast into a 2048² sun map fitted to the near view, drawn from a second walk of the
frame's merc and generic buckets before bucket 0; the world and the actors receive it and lose
only the sun's share of their light (sidecar k·v·max(0,n·L)·Lgt over the fitted light), so baked
shade is not darkened twice. Setting off is the vanilla path (FLIP within the master-to-master
noise). Cost, indicative only: ~0.05 ms GPU, 0.3 ms CPU per frame (1440p MSAA 4, not solo).

## Evidence
- Gallery (every shot, off / on): `$SANDOVER_DATA/shots/shadows/gallery.html`.
  Captures: `$SANDOVER_DATA/experiments/p2-shadows/v2/{off,on}`, manifests
  `sandover/shots/shadows-v1.yaml`, `shadows-lamp.yaml`.
- Parity, setting off, after the rebase onto master with the emissive layers: `sandover-parity
  check` on auto-v1 against the `f953354f0` goldens, 46/56 pass; the rest are one crashed
  instance (village1-hut-dusk missing, and village1-hut-night shot on the retried instance with a
  different clock, FLIP 0.026) and 8 `harness` shots whose sidecar lacked the camera (FLIP
  0.000000-0.00036). Re-captured, the four village1-hut shots give FLIP 0.000062 / 0 / 0 / 0
  (`parity/runs/20260924-022719-check-auto-v1/`, `experiments/p2-shadows/parity-v1hut/`).
- Gameplay parity (`sandover/shots/shadows-gameplay.yaml`, 600 frames at beach-start and
  jungle-start, every process-drawable's position printed by a stage hook): two off runs and two
  on runs. 164 of 170 actors match to the bit in every pair; the same six (a babak, a hopper,
  four jungle fish) differ between the two off runs as much as between off and on, so they are
  run-to-run noise, not the caster pass (`experiments/p2-shadows/gameplay/`).
- Cost (log line `actor-shadows: ... pre-pass`, 240-frame means, 3 instances sharing the GPU, so
  indicative only): GPU 0.04-0.10 ms, CPU 0.25-0.47 ms; 18 casters at village1-hut, 33 at
  beach-start, 62 at jungle-start. Not tuned.
- Sun direction per hour from the sidecar slots (log): 08:00 (-0.06, 0.18, 0.98), 12:00
  (-0.31, 0.95, -0.06), 16:00 (-0.14, 0.48, -0.87); global share 0.55-0.63 by day, 0.26 at 23:00.

## What works
- Long morning and evening shadows with full bodies; noon shadows under each actor; night
  shadows are faint because the moon is a small share of the light (consistent with the bake).
- Every merc and generic actor casts (Jak, Daxter, NPCs, enemies, orbs,
  crates, chests, platforms, level props). Sprites and particles (eco, sparkles) never cast;
  merc eyes and translucent or water draws do not either. Table in `kb/howto/shadows.md`.
- Caster reach: casters more than 8 m above the receiver (along the ray, scaled by the sun's
  elevation) cast nothing, so a floating orb does not throw a shadow far away. A first version
  measured the reach along the ray and cut Jak's shadow in half at low sun; fixed.
- Landing disc: a faint soft disc straight below Jak (0.3 strength, fades over a 20 m drop)
  replaces the stencil blob, so jumps read at any sun angle.
- Emissive: `tfrag3_sun` scales the shadow term by 1 − E from sidecar record.w bits 16-23 (the
  p2/emissive version-2 field; 0 in today's version-1 sidecars). Once that branch merges, use its
  per-source weighted `emissive` varying instead.

## The culling answer
GOAL culls actors per frame against the single view frustum in `dma-add-process-drawable`
(`drawable.gc`). With shadows on, `pc-shadow-caster-visible?` also accepts an actor whose
bounding sphere, swept 16 m along `*time-of-day-context* current-shadow`, touches the frustum;
such actors are drawn off screen and cast. The draw is flagged `*pc-shadow-only-draw*`: no
`was-drawn`, no LOD change, no matrix-engine queue, no closest-object or texture-mask update, so
game state is the vanilla one (checked, see Gameplay parity). Actors not spawned, behind the
camera plane, or beyond the level's draw distance still pop in.

## Limits
- One cascade: 40 m of the view at about 4 cm per texel; beyond it no actor shadows.
- The world does not cast onto actors (the bake has no geometry map); an actor walking into
  baked shade keeps its own sun light. Actors do self-shadow.
- Double shadows: the stencil shadow is skipped while maps are on; baked shade is protected by
  the sun share. Receivers without sidecar data (shrubs, wind and envmapped ties, generic,
  levels without a fit) use a global share and can darken baked shade a little.
- Lamps: a hardcoded list of the two village1 `hutlamp` actors; intensity from the flame slot
  (5) weight with a fixed warm colour and inverse-square falloff; lit only with time-of-day
  effects on. Lamp shadows are hard to see in the lamp shot. A light pre-pass should replace
  the list.
- Cutscenes and the title use other light groups; untested. Stereo: the pre-pass runs once per
  frame and both eyes read the same maps (untested live). Jak 1 only; Jak 2/3 skip the pre-pass.
- The rig's lamp shot moves Jak by editing the village1-hut continue point (the
  `village1-demo-convo` continue never reports ready in the rig).

## Foliage
Grass, bushes and trees are background (tfrag, tie, shrub) and do not cast. Their shadows on
the world are already in the bake, so casting them into the sun map would darken baked shade a
second time. What is missing is foliage shadowing actors (Jak walking under a palm stays lit).
That needs a second, world-caster map drawn from the tie and shrub trees and read only by the
merc receivers; it is a separate item, not part of this spike.

## Follow-ups
- World casters onto actors (foliage, trees, rocks) through a background-only map.
- DECISION (human): does the look earn the feature, and at what strength.
- Light pre-pass for lamps; merge with the emissive branch's E; cost measured solo.
