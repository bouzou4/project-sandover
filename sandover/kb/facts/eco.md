# Eco facts (Jak 1): vanilla reference

What each eco type looks like and does in vanilla, as the baseline for eco lighting work.
Engine claims pinned at `c5c5c4bba`; colours and locations come from the light census
(`$SANDOVER_DATA/lights/*.json`, `sandover/tools/sandover/lights/entity_types.json`).
Reference captures: `sandover/shots/eco-reference.yaml` (vanilla, noon 12:00 and night 22:00).
Why it matters: eco keeps vanilla's hue and shape, our work adds HDR headroom, bloom and the
lights eco casts. Compare hue against this table first.

**Vanilla eco lights nothing.** Jak 1 has no dynamic lights for eco. Vents, clusters, cells
and pickups are additive particles and self-lit models; the ground and walls around them keep
their baked vertex colour whatever the eco does. The only runtime world lighting in these
levels is palette-slot fades, and none of them is driven by eco (the darkcave glow crystals,
`cavecrystal`, are not eco; see `facts/levels.md` §4).

Status legend as in `facts/levels.md`: **verified** = read in source at the pinned sha;
**census** = from the light census tables.

## Table

Colour is the census particle colour (max channel = 1). The two measured columns are filled
from the vanilla captures of `eco-reference.yaml`: core = the brightest solid part of the
element, halo = the additive glow just outside it; both as linear 0-1 RGB at noon / night.
Halo hue night: measured hue of the night halo in degrees, vanilla / A (master before the baked-tint rule)
/ B, from the `cores-002` manifest notes.

| Eco type | Colour (census) | Where found (census) | Vanilla behaviour | What it lights in vanilla | Core RGB noon / night | Halo RGB noon / night | Halo hue night, deg (vanilla / A / B) | Reference shots | Status | Source |
|---|---|---|---|---|---|---|---|---|---|---|
| Blue | 0.42, 0.47, 1.0 | vents: beach, jungle, jungleb, firecanyon, ogre, sunkenb, swamp, training, citadel, finalboss; clusters: most outdoor levels incl. village1; blue-eco-charger (sunkenb) | vent: `group-part-vent-blue-active` particles while open; cluster: `group-eco-blue` orbs from its launch control; pickup: `group-eco-blue-collect` burst | nothing | | | 220 / 225 / 223 | `blue-vent-*`, `blue-cluster-*` | verified | `goal_src/jak1/engine/common-obs/collectables.gc:144-147,160,1642@c5c5c4bba`; `collectables-part.gc:61,218,335@c5c5c4bba` |
| Green | 0.7, 1.0, 0.05 (pill 0.5, 1.0, 0.1) | vents: beach (5, each under an `ecoventrock` until broken), rolling (1, open); clusters: green crates and pills everywhere | vent: `group-part-vent-green-active`; cluster: `group-eco-green` / `group-eco-green-pill`; pickup: `group-eco-green-collect` / `-pill-collect` | nothing | | | 103 / 114 / 118 | `green-vent-*` | verified | `collectables.gc:150-158,1652,1661-1666@c5c5c4bba`; `collectables-part.gc:1249,1359,1478,1496,1612@c5c5c4bba` |
| Yellow | 1.0, 0.84, 0.0 | vents: maincave (3), swamp, lavatube, citadel; capped until `snow-eggtop`: ogre, robocave, snow; clusters: swamp, lavatube, robocave, ogre, finalboss | vent: `group-part-vent-yellow-active`; cluster: `group-eco-yellow`; pickup: `group-eco-yellow-collect` | nothing | | | 76 / 75 / 89 | `yellow-vent-*` | verified | `collectables.gc:132-135,1657,1668-1681@c5c5c4bba`; `collectables-part.gc:883,1045,1173@c5c5c4bba` |
| Red | 1.0, 0.4, 0.0 | vents: snow (4), citadel (1); clusters and crates: misty | vent: `group-part-vent-red-active`; cluster: `group-eco-red`; pickup: `group-eco-red-collect` | nothing | | | | `red-vent-*` | verified | `collectables.gc:138-141,1647@c5c5c4bba`; `collectables-part.gc:515,642,722@c5c5c4bba` |
| Dark eco | 0.31, 0.09, 1.0 | pools: maincave (5), sunken (2), misty, robocave, finalboss; boxes (`crate-darkeco-cluster`): firecanyon (8), ogre; barrels: lavatube | pool: water-anim surface plus additive `group-dark-eco-pool-nasty` at random ripple vertices; box and barrel: texture only, purple burst on explode | nothing | | | | `dark-pool-*` | verified | `goal_src/jak1/engine/common-obs/dark-eco-pool.gc:152,328@c5c5c4bba` |
| Power cell | 1.0, 1.0, 0.6 | 26 placed cells (training, jungle, beach, misty, sunken, snow, ...) plus task rewards | body drawn with `color-emissive` 0.5 (self-lit, not lighting); `group-fuel-cell-starburst` flares; rises 0.7 m as Jak approaches | nothing | | | | `power-cell-*` | verified | `collectables.gc:864,917-920,1115@c5c5c4bba`; `collectables-part.gc:1751@c5c5c4bba` |
| Dark eco crystal | 1.0, 1.0, 1.0 (flash) | maincave (4), darkcave (1); task `cave-dark-crystals` | idle: unlit (`color-emissive` 0); flashes `color-emissive` 1,1,1 only during the activate countdown, then explodes (purple burst) | nothing | | | | `dark-crystal-*` (idle only) | verified | `goal_src/jak1/levels/maincave/dark-crystal.gc:413-425,529-531@c5c5c4bba` |

The pickup bursts and the dark eco crystal flash are transient and are not in the reference set
(the rig freezes on a settled frame); the captures show the resting state.

## How it should read (to be confirmed)

- Blue:
- Green:
- Yellow:
- Red:
- Dark eco:
- Power cell:
- Dark eco crystal:

## Emissive devices that are not eco

| Element | What it is in the game | Particles | Intended look | Status | Source |
|---|---|---|---|---|---|
| Snow bumper field (`snow-bumper`, 13 in snow) | A precursor switch on the ice. Within 90 m (xz) of Jak it draws a cyan repulsor field around itself and shoves Jak back when he comes within 5 m; touching it switches it off (perm complete), and the last one of a group spawns the `snow-bumpers` power cell | `group-snow-bumper-idle`: part 1893, 15 additive `hotdot` sprites, colour 0, 64, 64-96, 1.5 x 6 m, on a 5 m ring at 1.5 m height, respawned every 0.085 s | A glowing field (2026-10-09: "a shield-like repulsor on those precursor switches so they should be glowy things"): it keeps its emissive (hotdot) and lights the ice around the switch as a soft pool from the switch, not as discs under each sprite | verified (code); intent from play feedback | `goal_src/jak1/levels/snow/snow-bumper.gc:33-58,127-157,165-196@3c5242d4a` |
| Beach grotto light shafts (`beach-part-grotto-1`) | Faint pale-yellow haze cones from the grotto roof, launched only with the camera below the emitter or more than 30 m away | part 666, `flare` texture, colour 192, 192, 128, 15-20 x 5 m | Vanilla haze: not an emitter (the flare row's min_chroma keeps it out) | verified (code) | `goal_src/jak1/levels/beach/beach-part.gc:14-38,72-82@3c5242d4a` |
