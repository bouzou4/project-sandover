---
title: Light census - every light source per Jak 1 level, offline
date: 2026-09-25
issue: #191
status: verified (positions, sources) / inferred (additive flag, some colours)
sha: branch tools/light-census
---
## Answer (≤5 lines)
`sandover-lights census` lists 10,433 lights over 21 levels into `$SANDOVER_DATA/lights/`:
2,523 entity, 543 particle, 7,304 static, 63 vanilla-light. Every record has a position (m),
colour, radius hint, an on/off state (`always`, `entity-flag`, `task`, `time-of-day`, `player`,
`transient`) and its source row or GOAL line. Entity emitters are fixed at their entity
position; only small bobbing (orbs 0.25 m, cells rise up to 0.7 m near Jak) and a few platforms move.

## Evidence
- Entity table: `sandover/tools/sandover/lights/entity_types.json`, 315 actor types read from
  goal_src: 31 emit, 40 maybe, 244 do not. Pickup enum `engine/game/fact-h.gc:48`. Vent gate
  `engine/common-obs/collectables.gc:1661-1681`. Crate contents `engine/common-obs/crates.gc:767-857`
  (no eco-info drops eco-pill-random; 'darkeco and 'bucket drop nothing).
- The maintainer's recollection holds: vents and eco sit at the entity position and change state
  only (vents are never used up; yellow/red/blue eco respawns when the player's eco runs out).
- Particles: 1,103 part spawners; 543 point at a group with an additive glow part, 531 at
  smoke/water/rock groups, 27 have no art-name (spawn nothing), 2 name groups not in goal_src
  (`group-beach-grotto-1` is handled by a special state, `group-ogre-hotstone`).
- Vanilla lights (`vanilla_lights.json`): merc gets no point lights. Each draw uses one light
  group of 3 directional lights plus ambient (`engine/draw/drawable.gc:500-548`), chosen by
  the actor's `light-index` (`process-drawable.gc:373`; 0 sun, 1-7 level kits, 255 = the
  player's group). "Positional" light is faked in `update-mood-<level>` (`engine/gfx/mood/mood.gc`)
  by distance to hard-coded points: 63 positional entries (village1 hut kits, village2 lamps,
  rolling spheres, darkcave crystals, village3/ogre lava, citadel/finalboss under-glow) and
  103 directional ones that stay in the JSON only.
- Statics: emitter-scored textures (176 of 671), lit triangles clustered on a 2 m grid, tiles
  of 12 m. Sunken, firecanyon and citadel dominate (glowing precursor trim, coal, lava floors).
- Spot check: no new capture (game instances paused). Jungle lights projected into the
  existing `tile-lights` captures: the ecovent lands on the vent bowl at 11 m and the
  tower-top fuel-cell on the cell at 10.7 m (thumbnails `01-spot-*`). Village1 has no
  near-field capture; cross-check there is data-only: the 13 `hutlamp` actors and the 26
  `vil-lamp` static clusters are distinct lamps 8-23 m apart, not duplicates. A village1
  capture check stays pending.

## Implications for the design
- The list is too long to upload whole: cull by distance to the camera and state, nearest N.
  Statics are the bulk; lava and precursor trim want merging into area lights or a lower cap.
- Runtime state the producer needs: entity perm status (dead/complete) per entity, game-task
  status, player blue eco, and the hour. Positions never need publishing except for the
  `moves: true` platforms and bosses.
- Colours for money, cavegem, hutlamp and lava meshes come from textures, not code (assumed).
- Additive = `(:flags (... bit3 ...))` on a part is inferred from the textures that carry it.

## Follow-ups
- Village1 spot check against a capture when game instances are allowed again.
- Emitter-score false positives (pillows, bookcovers in village1) come through as statics.
- 40 `maybe` types need a look (platform under-glows, launchers).
