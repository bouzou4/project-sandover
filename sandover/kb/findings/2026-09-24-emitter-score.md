# Emitter score, true emitters vs lit receivers

Date: 2026-09-24. How-to: `kb/howto/materials.md` "`sandover-materials emitter-score`".

## Problem

Bake-derived E is light from anything but sun and ambient, so a wall lit by a lamp carries E
as well as the lamp: 503 of the 671 emissive maps cover their whole texture and 185 defaults
sit at the torch level. The false positives were confirmed to be are receivers of baked
secondary light, not detection error, and that they are hard to judge one by one.

## Score

`score = sigmoid(bias + sum(w * c))` over six components (preset `presets/emissive.toml`):
correlation of E with albedo luma (+2), coverage (-1.5), per-triangle E falloff (-3), fraction
of the texture's triangles that are lit across all levels (+3), dominant slot prior (+2) and
E-weighted albedo luma (+3), bias -1. The weights were set by hand; a logistic fit on the weak
labels below gives the same signs and ranking (falloff, consistency and brightness strongest,
coverage weakest) and a slightly lower AUC (0.927 vs 0.934 for the hand weights).

## Validation

Ground truth is thin: `emissive-overrides.csv` has four verdicts, three on textures with a map
(`bluelight` emitter, `gen-metal-dark` and `cmn-precursor-metal-edging-01` receivers), and
`emissive-strengths.csv` does not exist yet. At threshold 0.5 all three are right
(bluelight 0.67, the receivers 0.21 and 0.29): precision 1, recall 1. The highest threshold
keeping bluelight is 0.66; 0.5 is kept so one point does not set it.

A name-based weak label set (458 textures: 65 emitter names such as torch, lamp, lava-flow,
crystal, glow; 393 receiver names such as rock, wall, wood, metal, plank) gives precision 0.52,
recall 0.88 at 0.5 (AUC 0.93). The misses are mostly names that are not emitters (the cave
torch's wooden handle `cv-torch-02`, glass frames) plus the eco anchor (0.25, kept as an
emitter because it is an anchor). The false positives are receivers that are lit everywhere
they are drawn: firecanyon rock and metal under the level-wide lava slots, maincave statues
under the crystal fade-lights, sunken hot pipes. Consistency cannot tell those apart; a
per-level normalisation (lit fraction relative to the level's own) is the next lever.

## Result

494 of 671 textures fall below 0.5 and default to the rail bottom (1/64); 356 of the 503
full-coverage maps and 114 of the 185 torch-level defaults. By material class (receivers /
total): metal 202/252, stone 105/133, wood 110/124, foliage 31/32, organic 20/26, special
26/104. The top-24 sheets (`$SANDOVER_DATA/materials/emitter-score/sheets/`) show
lamps, glass, precursor glow panels, lava and eco valves on the emitter side and grass,
bark, wood, metal plates and stucco on the receiver side, with no visible cross-over.

## Limits

- Only 3 human labels: the threshold is not yet validated; rerun `--validate` once the ladder
  has a few dozen human rows.
- The Emissive tab writes `default` when a row is accepted at its default, and
  `export-sidecar` only scales E for `human` rows, so accepting a receiver at the scored
  default does not dim it in the sidecar until the tab compares against `bake_default`.
- Components (c) and (d) skip shrubs, like the emissive table.

## Export: receivers scaled in the sidecars (follow-up)

`sandover-unbake export-sidecar` now multiplies the E of every receiver by `receiver_scale`
(`presets/emissive.toml` `[emitter_score.export]`, default 0). Human ladder rows and overrides
win, and anchors and human emitters are never scaled. The export also writes unscaled sidecars
to `lighting/raw/` for the emissive tools. At 0.5, 492 textures were scaled: the 494 receivers
minus the two with an override. All 21 levels were regenerated and installed; the previous
sidecars are in `lighting/retired/20260924-pre-receiver/`.

Vertices with E > 0: beach 25 334 -> 5 279, village1 52 030 -> 10 121, sunken 592 925 -> 481 475
(sunken's caustic slots carry weight 0 at runtime anyway).

Rig captures at 2560x1440 MSAA 4, night, effects on, SSAO `deep`, bloom on. Before = the
same export with `--no-receiver-scale`. Changed pixels (> 2/255): beach lamp 0.03% (the lamp
post loses its bloom, the lantern panel keeps it), village1 hut 0.07%, sunken start 17.7%
(mostly the animated water and fish in the window). In the E debug view (view 8), pixels
at E > 0.1 fall from 2.5% to 2.5% on beach, 2.6% to 2.3% on village1 and 3.9% to 3.0% on
sunken. The visible change at these spots is small, because most receiver E already sat
below the bloom ramp start (0.1). A shot of the village1 lamps with a custom camera did not
repeat between runs and was dropped. The comparison is A before, B after, with the view-8 pair per spot.
