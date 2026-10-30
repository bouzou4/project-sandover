# Effect slots as an emissive mask (SSAO exclusion, bloom)

Date: 2026-09-23. Effect slots as emissive layers. How-tos:
`kb/howto/lighting.md` §1b, `kb/howto/ssao.md` (Emissive mask, Bloom).

## Result

- The beach lamp's lantern panel no longer receives AO: with `deep` on true normals it matches
  SSAO off inside the lantern and keeps contact AO on the post base and the sand.
- The mask is a per-vertex value E in the `.unbake` sidecar (version 2, all 21 levels), written
  per frame to an R8 attachment by opaque tfrag/tie and read by the SSAO composite
  (`AO × (1 − smoothstep(0.1, 0.4, E))`, background pixels only).
- A bloom from the same mask (`pc-set-bloom`, off by default; strength 1, radius 6, threshold
  0.3, gated by the same E ramp) adds a visible halo to the lamp panel, lava and bright additive
  particles, and nothing where E = 0. Additive world sprites write their brightest channel × 0.4
  into the mask (large smoke stays under the ramp, bright cores exceed it); HUD sprites clear it.
  Bloom runs after bucket 66 (sprites), before the HUD buckets.

## What E is, and a correction to the original specification

The original specification defined E as the effect-slot contribution. That alone misses the lamp case (AO darkens emissive lamp surfaces at night): **beach slot
5 (flames) is all zero** (the unbake fit already noted "beach slot 5 has no lit vertices"). The beach and
village1 lamps are baked into the night sun slots 6-7. E therefore has three sources:

| Source | Finds | Runtime weight |
|---|---|---|
| effect slot luminance | village1 flames, firecanyon lava, darkcave crystal pools, misty/snow/citadel flames | slot weight if the role is an emitter; lightning, caustics and camera fades weigh 0 |
| night excess over a moonlight prediction (`night_ratio` × brightest day slot + 32 u8) | lamps baked into night slots (beach, village1, jungle, finalboss) | summed night-slot weight |
| light on at every hour (dimmest sun slot above 96 u8) | lanterns lit by day too | 1 |

Two first attempts failed and are worth knowing:

1. Night luminance above the vertex's brightest day slot: the lit sand around a beach lamp is
   brighter at noon (205) than at night (142), so the lamp pool and part of the lantern got
   E = 0. The moonlight prediction uses the level's median night/day ratio (0.43 beach, 0.48
   village1) instead.
2. A median-luminance test for "night slot" flagged dusk (slot 4) on beach and morning slot 0
   on firecanyon; night slots now come from the hour table (live at 0:00, not 10:00-14:00).

The margin was raised from 16 to 32 u8 after the first beach E view showed faint E (0.03-0.08)
on distant cliffs (it was noticed as artifacting on the far mountains). That value sat
below the ramp start and never changed AO; at 32 it roughly halves (view-8 grey p90 14 → 7 of
255 over that band, which includes the dimmed scene).

## Measurements

E on the beach lamp at 23:00 (view 8, grey level ≈ E): lantern panel 0.5-0.57, frame 0.3-0.4,
post ≈ 0.05, lamp pool on the sand ≈ 0.03. So the panel is fully excluded, the post and the sand
keep their AO. Share of vertices with stored E > 0 per level: 5-20% on the village1-table
levels, 50-85% on firecanyon, sunken(b), swamp and village2 and 25% on lavatube, where
lightning/caustic layers (weight 0 at runtime) and lava-lit rock dominate. Per-level
histograms: `$SANDOVER_DATA/lighting/v2/<level>-sidecar.json`.

Changed pixels, `deep` with mask vs `deep` with E ignored (> 2/255): beach 0.04% (max 101),
village1 night 0.01%, firecanyon-start 1.7%, firecanyon-end 5.0% (lava-lit ground at E 0.1-0.3
gets partial AO), village1 noon 0.00% (max 9), darkcave-start 0.02% (no crystal is lit at the
start). Bloom vs no bloom (final settings): beach 0.07% (max 94), village1 night 0.13%,
village1 night with particles 0.27% (max 201), firecanyon-end 8.8%, with particles 25% (the
lava smoke plume), village1 noon 0.06%: those noon pixels are all on the windmill sail, which
animates between runs; bloom adds nothing in daylight without an emitter. A first setting
(strength 0.6, raw E) was judged too subtle on the lamp and lava.

Captures: `$SANDOVER_DATA/shots/emissive/{off,deep-noE,deep,deep-bloom,eview}/`, manifest
`sandover/shots/emissive.yaml` (two shots with particles on), sheets (SSAO off / deep, E
ignored / deep + mask / + bloom / E view) in `.../emissive/sheets-final/`, with a lamp crop for
beach and `overview-off-deep-bloom.png` (off / deep / deep + bloom per spot). village1-hut's lamps are small at
that continue point; village1-warp failed to stage in the rig and was dropped.

## Parity

Master baseline: origin/master (the normal-target merge) built in the same tree, master
shaders, version-1 sidecars for village1 and beach. FLIP mean per shot (`sandover-capture compare`):

| Pair | beach | village1 night | village1 noon | firecanyon start / end | darkcave |
|---|---|---|---|---|---|
| off vs off (repeat, noise) | 0.00003 | 0.0001 | 0.0003 | 0.005 / 0.036 | 0.0001 |
| off vs master off | 0.00002 | 0.00003 | 0.0002 | 0.005 / 0.019 | 0.0001 |
| deep, E ignored vs repeat | 0 | 0.000004 | 0 | 0.011 / 0.025 | 0.0001 |
| deep, E ignored vs master deep | 0 | 0.00001 | 0.00001 | 0.013 / 0.017 | 0.0038 |

SSAO off matches master; `deep` with the mask off matches master bit-for-bit on beach and
within the repeat noise on village1 (measured). Firecanyon differs by animated lava within its own noise. Darkcave differs because
master had no sidecar there (face normals) and this build has version-2 normals, not because
of the mask.

## Cost (recorded only; not solo, other rig instances were running)

2560×1440 MSAA 4, 240-frame windows, ms:

| Spot | buckets, E off / on / + bloom | composite, E off / on | bloom |
|---|---|---|---|
| beach 23:00 | 2.17 / 2.25 / 2.44 | 0.095 / 0.132 | 0.141 |
| firecanyon-end noon | 2.54 / 2.61 / 2.76 | 0.119 / 0.147 | 0.140 |

The mask costs about 0.07-0.09 ms in the opaque buckets plus 0.03-0.04 ms in the composite;
bloom 0.14 ms (measured before bloom moved after the sprite bucket; same passes). Memory: 15 MB for the R8 mask, 22 MB for bloom's resolve and two half targets.

## Limits and follow-ups

- Shrubs, generic, wind and envmapped ties write no E and keep the value of the emissive
  surface they cover (grass in front of lava). Characters are excluded in the composite but
  not in bloom: a character over a lamp or lava keeps its E and blooms (inferred from the code).
- Only background tfrag/tie and additive sprites carry E; lamp actors and merc glow do not.
- Lava-lit rock gets partial AO exclusion because the lava slots light it; if that reads as
  flat, raise the ramp start or restrict effect-slot E to its excess over the slot's median.
- Bloom uses the RGBA8 frame; with the FP16 target the threshold becomes an HDR level.
- A worktree root without links needs `game/` (shaders, assets), `out/jak1/{fr3,iso,obj}`,
  `goal_src`, `custom_assets` and `decompiler/config` copied in; with only the first two gk
  hangs before the kernel boots.
