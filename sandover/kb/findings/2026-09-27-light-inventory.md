---
title: Which things look like lights but light nothing, and which should
date: 2026-09-27
status: inferred
sha: crystal-lights (on local-lights)
---
## Answer (≤5 lines)
`sandover-lights inventory` lists every light-looking thing per level against the world light
table: 21 levels, 598 rows (`$SANDOVER_DATA/lights/inventory/inventory.md`). Three gaps: (1) props
with no baked E (ice crystals, cave mushrooms, blue cave crystals, precursor glow overlays, vent
fire) have no census producer at all; (2) statics named as lights (rolling's `rol-light` posts,
`fin-light`) lost the shared static cap to lit floors; (3) the dark eco crystals and boxes were
`maybe`. Added: 15 prop textures (1155 lights), a named-light static pool (+351 statics), 5 dark eco
crystals and 9 dark eco boxes. Not captured in game yet: each type has a shot in
`shots/crystal-lights.yaml`.

## Method
- Census rows (entities, statics, part spawners) against `bake_rows`: lit = in the table,
  else the bake's reason (`maybe`, `emits: false`, cap, non-emitter slot).
- Background textures whose name reads as a light (`GLOW_NAME` in `lights/inventory.py`) and that
  have no baked E, so no census producer sees them: geometry clustered on a 1 m grid, and the
  median distance from a cluster to the nearest table light (a lamp's wood next to its lamp
  row is already lit).
- "Looks like a light" for those textures: median vertex colour (brightest palette slot) x
  texture luma, as a percentile of every background draw of the level. Read with the texture
  (contact sheet) and the play frames (0019 ice crystal, 0021 mushrooms and blue wall crystals).

## Classified list

### Add (this PR)
| What | Levels (count) | Evidence | Light |
|---|---|---|---|
| Ice crystals `snow-crystals` (shrub) | snow 144 | play report, 0019; 95th pct | glow, texture colour 0.48, 0.69, 1 |
| Blue cave crystals `cv-bluecrystal` | maincave 24, robocave 23 | 99th pct; white-blue on the walls in 0021 | glow |
| Cave mushrooms `cv-mushroom-01-decal-01`, `cv-tallshroom-decal-01` | maincave 99+60, robocave 50+40 | bright green in 0021; 79-92nd pct | glow, green from vertex colour |
| Teal mushrooms `ogr-mushroom-whole-01`, `ya-mushroom-whole-01` | ogre 45, rolling 131 | 99th / 93rd pct | glow |
| Village3 mushrooms `vil3-mushroom-01/02` | village3 33+35 | 80-88th pct | glow |
| Giant crystals without E `vil3-giant-crystal-nt-01/03` | village3 17+8 | 96-99th pct; neighbours `vil3-crystal-01` are statics | glow |
| Precursor glow overlays `jngb-precursor-circuitpattern-andwall-02-glow`, `jngb-precursor-spheres-glow` | jungleb 23+94 | 87-93rd pct, blue vertex colour | glow, gain 0.3 |
| Pink beams `cit-pinkbeam` | citadel 82 (12 m tiles) | 98th pct | glow, gain 0.3 |
| Vent fire `lava-vent-fire` | lavatube 236 (12 m tiles) | 99th pct | glow |
| Blue precursor lights `lava-precursor-bluelight` | lavatube 11 | 99.9th pct | glow |
| Dark eco crystals `dark-crystal` | maincave 4, darkcave 1 | play report, 0021; flash `dark-crystal.gc:417-425` | strong x 2.5, 8 m, purple, gate 15 |
| Dark eco boxes `crate-darkeco-cluster` | firecanyon 8, ogre 1 | dark eco texture, no emissive in code | strong x 0.3, purple, until it explodes |
| Statics named as lights (`rol-light`, `rol-light-s`, `fin-light`, precursor `-glow` trim, `jng-light-blue`) | rolling 128, finalboss 89, citadel 119, others | baked E emitters dropped by the shared cap of 48 (rolling kept 47 of 177) | own pool, `named_cap` 128 |

### Already lit
Lamps and lanterns (`vil-lamp`, `bch-lamp`, hut lamps), lamp wood and fireplace rims (a lamp row
within 1 m), torches (all `*-torch` part groups), flame pots, cave crystals, vents and valve
insides (row within 1 m), power cells, eco and eco crates, lava (entities, lava statics, ogre and
village3 lava particles), sunken heat pipes (particles), `sun-hot-coil` (row within 1 m),
`cav-firebarrel-flame-01` (1.6 m), maincave `cv-torch-02` (1.5 m, the torch flames).

### Should not light
| What | Why |
|---|---|
| Precursor orbs, orb crates (1361) | design decision 2026-09-26 |
| Scout fly crates `crate/buzzer` (110), orb caches (14) | not self-lit |
| Launchers (22), platform under-glows (`plat`, `plat-eco`, `citb-*`, flut flut, orbit, qbert: 106) | glow only with blue eco (player-gated) or move; the table is static |
| Racer and flut flut pads (13), periscopes (5), reflector mirror, muse, sequenceC, assistants' torches, pelican | small, moving or transient particles |
| Bosses and boulders (`ogreboss`, `robotboss`, `sage-finalboss`, `ecoclaw`, `fireboulder`, `shortcut-boulder`, `darkecobarrel`) | moving; faint `color-emissive` 0.125; later if asked |
| `sunken-water` (13), eco from spawners/crates dropping into the entity pool | area source / no entity to follow |
| Statics on lightning slots (village1-2 `bluelight`, `vil2-pinklight`, `swp-launch-light`, 847 in all) | they glow only with the lightning flash |
| Statics on fade-lights, fade and caustics slots (darkcave walls, sunken trim) | receivers of the crystal light, or shimmer |
| Remaining generic statics beyond the cap (firecanyon coal 1133, sunken circuitry 1101, citadel glass 446) | lit floors and trim, dim per cluster; the cap is the budget |
| `robocave cv-torch-02` (82) | unlit wooden torches: 8th pct, no flame near |
| `sun-mushroom*`, `sun-glow-01`, `vil2-glow-01`, lavatube hot ground, `vil3-lava-top` | 14-66th pct: not brighter than the level |

## Implications for the design
- New census kind `prop` (`prop_lights.json`), bake class `glow`: tile intensity
  `world_glow` 0.12 and actor power 0.5 (strong 0.8, lamp 1.2), times each texture's gain
  (snow crystals 1.0, cave props 0.4, overlays 0.3). The first capture (crystals-001: tile 0.03, actor
  0.2) changed the snow 1 m from the crystal by +0.054 lum, as the offline model predicted (+0.055).
  The tile pass takes the 16 nearest other lights as before and up to 8 props and dark eco
  crystals in their own slots, and those never suppress the screen tiles near them (they replace
  no emitter).
- The actor lights rank everything by the light at Jak, so props enter the 4-light / 2-shadow
  budget only when nothing stronger is near; their shadows are weak because the shadow term
  removes the light's share of the receiver's light.
- Dark eco crystal: 10% idle, 100% on each 0.1 s flash of the countdown (flash delays 0.6 to
  0.01 s, `*dark-crystal-flash-delays*`), a 0.4 s fading burst on exploding, then off.

## Follow-ups
- Capture each type (`shots/crystal-lights.yaml`) and check colour and strength against the
  vanilla frame, especially the saturated vertex-colour hues (jungleb blue, cave green).
- Moving emitters (platform under-glows, bosses) need a dynamic table row.
