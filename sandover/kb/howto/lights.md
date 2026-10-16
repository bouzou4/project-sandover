# Light census how-to (issue #191 phase 2)

Offline list of every light source per Jak 1 level: the input for the off-screen light list
(world-space lights that keep lighting the floor when their emitter is off screen). The engine
reads a baked table of it (`bake`, below), never the JSON. Results of the first run:
`kb/findings/2026-09-25-light-census.md`; the engine side:
`kb/findings/2026-09-25-world-light-list.md`.

## Run

```sh
cd sandover/tools
uv run --no-sync python -m sandover.lights census            # every unbake level
uv run --no-sync python -m sandover.lights census --levels village1 jungle --no-static
uv run --no-sync python -m sandover.lights inventory          # what looks like a light, lit or not
```

Console script `sandover-lights` once the venv is synced. Inputs: the entity tables
(`decompiler_out/jak1/entities/<level>-actors.json`), `goal_src/jak1` (particle groups,
game-task enum), the level glbs and `.unbake` sidecars under `$SANDOVER_DATA`, and
`$SANDOVER_DATA/materials/emitter-score.json`. About a minute for all 21 levels.

Output under `$SANDOVER_DATA/lights/`:

- `<level>.json`: a list of light records (below);
- `summary.md`: counts per level and kind, state kinds, entity types, what was skipped;
- `sheets/<level>.png` and `sheets/00-overview.png`: top-down scatter over the tfrag footprint
  (x right, z up), coloured by kind.

## Record

| field | meaning |
|---|---|
| `id` | `<level>:<entity name>` for entities and part spawners, `<level>:static:<texture>:<n>`, `<level>:vanilla:<n>` |
| `kind` | `entity`, `particle`, `static`, `vanilla-light`, `prop` |
| `type` | actor type (crates: `crate/<pickup>`), particle group, texture, or the vanilla light's name |
| `pos` | metres, game world space (the entity tables' unit; raw units / 4096) |
| `colour` | RGB 0..1, or null when the source has no colour of its own |
| `radius_hint_m` | reach to start from, not a fitted value |
| `state` | `{kind, var}`, see below; tasks also carry `task` (game-task name) |
| `source` | the row (`<table>#<row index>`), glb tree, or `goal_src` line |

Extra fields by kind: entities `aid`, `moves` (false, `small` for bobbing pickups, true),
`type_source` (the GOAL definition), crates `contents`; particles `group_source`, `textures`,
`intensity` (brightest light part, max channel / 128 x alpha / 128); statics `energy` (E x
area, m²), `tris`, `extent_m`, `role` (mood slot), `e_source` (sidecar E source with the most
energy: 0-7 palette slot, 8 static, 9 night); vanilla `vanilla_kind` (`positional` or
`region-kit`), `affects`, `light_group`, `y_from_ground` (the game tests xz only, y is the
footprint's median height there), `gain` (colours above 1 are divided by their max).

## State

What turns a light on or off, as the runtime producer will need to publish it:

- `always`: static geometry, part spawners without an hour mask, always-on devices.
- `entity-flag`: the entity's own status: a pickup collected (entity perm dead), a crate
  broken (its contents only appear then), a vent opened, a crystal hit.
- `task`: a game task: power cells (`<task> closed`), scout flies (fly n of `<task>`), devices
  that switch on when a task closes.
- `time-of-day`: a part spawner item with an `:hour-mask`. `var` lists the hours whose mask bit
  is set, which are the hours the item is off: the launcher skips it then
  (`sparticle-launcher.gc:746`). The village butterflies run 6:00-16:59.
- `player`: depends on the player (launchers glow only while Jak holds blue eco).
- `transient`: a short event-driven flash (cave crystals when hit, sunken water).

Vents get their gate per entity (collectables.gc vent init): `options` 5 (blocked + valve) waits
for the eggtop task of its colour, an `alt-actor` waits for that entity's perm complete,
`options` 1 alone waits for a script `'show`; otherwise the vent is always on.

Entities flagged `maybe: true` (platform under-glows, launchers, lamps whose hue is only in the
texture) are listed but unconfirmed; filter them out for a strict list.

## How each producer classifies

- **entity**: `sandover/tools/sandover/lights/entity_types.json`, one entry per actor type with
  `emits` (true / false / "maybe"), colour, radius, state and the GOAL source line. Colour
  `from eco-info` types (eco, vents) take it from the entity's `eco-info` pickup type
  (`pickup_emit`). Crates count only when their `eco-info` holds something that glows.
- **particle**: a part spawner's `art-name` names a `defpartgroup`; it is a light when a part
  is additive (`(:flags (... bit3 ...))`, inferred from which textures carry it) on a glow
  texture (water spray, drops and wings excluded). Colour = alpha-weighted mean of the light
  parts, 128 = 1.0.
- **static**: tfrag/tie draws of textures with `emitter: true` in emitter-score.json. Lit
  triangles (corner sidecar E >= `covered`) are clustered on a 1 m grid, clusters wider than
  8 m cut into 8 m tiles. Position = E x area weighted centre, colour = the texture's mean
  albedo over its emissive texels. Emitter-score false positives (a pillow, a bookcover)
  come through as statics.
- **prop**: `prop_lights.json`, background textures that look like lights but carry no baked E, so
  no other producer sees them (snow ice crystals, cave mushrooms, blue cave crystals, precursor glow
  overlays, lavatube vent fire; chosen from the inventory, `kb/findings/2026-09-27-light-inventory.md`).
  Every tfrag/tie/shrub draw of a listed texture is clustered on `cell_m` (default 1 m), one light
  per cluster. Colour: the texture's linear mean times the draws' vertex colour (`colour_from:
  texture` keeps the texture alone). Records carry `gain`, `extent_m`, `verts` and no `textures`,
  so an older bake takes them for a part spawner without light and skips them. The bake keeps a
  prop texture only where the game draws it as a light (`props.glows_in_vanilla`, from the
  `vanilla` measurements in `prop_lights.json`, medians over the frames): contrast >= 1.3 in a
  vivid colour (saturation >= 0.6), or half its pixels at the SDR ceiling and twice the
  surroundings' share. Measure a vanilla rig frame with `sandover-lights glowcheck FRAME.png
  --levels LEVEL` (z-buffers the background into a texture map at the rig projection, then per
  texture: median luma over the median of the other textures within 5 m, and the ceiling shares;
  Jak and nearby actors masked). A texture with no measured frame gives no light.
- **vanilla-light**: `vanilla_lights.json`. Jak 1 lights merc with three directional lights and
  an ambient per light group; positional behaviour is faked in `update-mood-<level>` by
  distance to hard-coded points. The positional entries are listed; directional ones stay in
  the JSON only.

## Bake: the engine's world light table

```sh
uv run --no-sync python -m sandover.lights bake            # every census level
```

Writes `game/graphics/opengl_renderer/WorldLights.inc`, which is committed and compiled into gk
(like `NeighboursMood.inc`), so a copied project root cannot silently lose the world lights.
Rebake and commit it after a census run or a change to `[emissive_lights.bake]`;
`tests/test_lights.py::test_world_lights_inc_is_current` fails while it is stale (and skips
the comparison where no census exists). `--csv FILE` also writes the rows as CSV. Row: `level, x,
y, z, r, g, b, radius_m, gain, cls, gate, param, aid`; code and gate list in
`sandover/lights/bake.py`, engine side in `game/graphics/opengl_renderer/WorldLights.h`.

Left out: entity types marked `emits: false` in `entity_types.json` (and crates whose contents
are), read by the bake itself so a type decision applies without a census rerun: precursor orbs
(`money`, `crate/money`: 1361 rows on 2026-09-26) are collectible icons, not lamps; their
vanilla glow and bloom stay. Also `maybe` entities (a type since marked `emits: true` keeps
its rows, and its `bake` entry can set class, gate and param: village1's 13 `hutlamp`s are
lamps gated by the flames slot 5, #237), vanilla lights (the game's own region kits), part spawners with no
light part, crates whose drops go to the entity pool (`eco-pill-random`, `eco-green`: nothing
to follow), statics on lightning/caustics/fade slots, statics on `fade-lights` slots (the cave
walls the crystals light: receivers, the crystal entity is the light), and statics beyond
`static_cap` per level (biggest energy first). Lava clusters (role `lava`) have their own
`lava_cap` (256, i.e. all of them): fire canyon has 172, and with the shared cap of 48 only 42
were in the table, so most of the lava lit only from screen tiles, which flickered with camera
motion (snapshot run 2026-09-25). Lamps (role `night`) have their own `night_cap` (64): village2's 47
biggest statics were lit stone-floor tiles (energy 1.6-53), so all 11 lantern posts (energy 1-2.2)
fell under the shared cap and lit nothing (snapshot run 2026-09-26, 0013). Statics whose texture is
named as a light (`light|lamp|glow`) have their own `named_cap` (128): rolling kept 47 of its 177
`rol-light` posts under the shared cap (#267). Citadel lamp glass (`^cit-glass`: the shield lamps, 260
clusters; other levels' glass stays a plain static) has its own `glass_cap` (512, all of it) and bakes as class weak x `glass_gain` (4608: on the 0022 camera
the wall around the lamps takes as much added light as from the tile lights it replaces), so it takes over its tiles but stays out of
the actor lights. Props have `prop_cap` (256). The table has priority over the tiles near it (`kb/howto/ssao.md`). The tile pass lists the `world_count` nearest rows that are on, except that a
row with one of the last readback's tile candidates within its radius for 8 frames
(`kClaimInFrames`) claims it, held 30 frames after the last one (`kClaimFrames`), and the nearest
`world_count`/2 claimed rows rank first: a far lamp wall the screen already lights takes its tiles
over instead of losing its slot to nearer rows that light nothing on screen, and at least half the
slots stay nearest-first. A camera cut clears the claims. Pan bursts log `EMCLAIMS` (claimed, ranked
first, unclaimed rows within 8 m left out).

A type's `bake` entry in `entity_types.json` takes `class`, `gate`, `param`, and optionally
`gain`, `colour`, `radius_m` and `offset_m` (added to the entity's position: the dark eco crystal's
root is at its base).

Class `glow` (props): tile intensity `world_glow` (`pc-set-emissive-lights 18`, 0.3, gfx.h and
`presets/emissive.toml`); actor power is the same `world_glow` x emissive intensity; both times each
texture's `gain` in `prop_lights.json` (the dark eco crystal's bake gain is 6). Every prop is a point light at its
cluster's centre; mushroom patches and crystal clusters cluster on a 2 m grid in 12 m tiles, so a
patch is one light and its one shadow map shadows all of its light. These lights (and the dark eco
crystal) fall off as 1 / (1 + d^2 / 1.8^2) windowed by (1 - d^2 / r^2)^2, on the ground
(`emissive_light.frag`, own soft limit 1.2) and on actors alike (`kAddedR0M`, actor power =
`world_glow` x emissive intensity), so an emitter dominates its first metres. The dark eco crystal, a 4 m
tall emitter, uses r0 3 m (`kWideR0M`; the tile pass reads it from the world light's alpha).
Merc level props (chests, crates; buckets `MERC_TFRAG_TEX_LEVEL0/1`) are marked with stencil bit 5
while local lights are on: the tile pass skips these lights there and merc gives them the light,
multiplied by their own texture like every merc surface. The tile pass (`world_lights::select`)
takes the nearest `world_count` other lights exactly as without them, plus glow props and dark eco
crystals in slots of their own (dark eco crystals: `kDarkSlots`, nearest first). Glow props are ranked
by what they add to the view, not by distance (fix/light-popin: the 8 nearest at full power made
patches 20-50 m ahead switch on as Jak walked in): score = power x in-view (the lit sphere against a
cone holding the frame, a 0.8 rad soft margin) x (r / d)^2; they take every slot the others leave
(14 with `world_count` 16), fade by camera distance over 0.55-1 x `(pc-set-emissive-lights 35)`
(70 m; 0 = the old 8 nearest at full power), and each kept one is weighted by its score over the
best unchosen one's (0 at 1x, full at 2x), so a light crosses the cap at zero power. They take
a fixed share of the world slots: `kMaxLights` - `world_count` - `kDarkSlots` - 2 spare - the 4
eco shot slots (`kGlowSpare`, `kEcoReserve` in EmissiveLights.cpp; 8 at `world_count` 16, as
many as before), leaving the tracker room for rows still fading out so a new row never evicts a
light that is still lit (when every slot is taken, the tracker reuses the unlisted slot drawn
faintest, not the one with the lowest fade). The cap does not move with those rows: a moving cap
drops the light at it above zero power. The
tracker slews their power by at most `Params::ramp` (6% of each channel) a frame instead of
fading them in; one listed again while fading out fades back up. `(pc-set-emissive-lights 36 n)` caps their slots (0 = all free; 8 = master's GPU
cost); `37` restores both defaults (sent by `sandover-settings-reapply`). These lights do not take over the
screen tiles near them (`LightTracker::World::suppress`): they replace no emitter, and suppressing the
tiles of the green cave crystals beside them left a corridor 8-15% darker than with them off
(crystals-005). The actor lights
keep one non-crystal glow prop at most (a patch of mushrooms is one light); crystals
(`crystal: true` in `prop_lights.json`, baked as param bit 1, `kPropCrystal`) always count.
`(pc-set-emissive-lights 19 0)` switches the glow props and the dark eco crystals off in both the tile
pass and the actor lights, for a same-build A/B; everything else keeps its light and its slot.
A dark eco crystal answers 1 (of 1000) while dark: the row stays listed at zero power
(`world_lights::listed`) in tile slots of its own (`kDarkSlots`, 2, beyond the props' 8), and both
the tile tracker (`LightTracker::World::snap`) and the actor lights (`fade_lights`) skip the fade
for it: its light is exactly GOAL's answer of the previous frame, full on a flash frame and zero the
frame after.

Radius: the census hint, raised to a per-class floor (`radius_min`, lamps 7 m: a lamp hangs 2-3
m over the street, and a 3-4 m radius barely reached it) and capped at `radius_cap_m` (12 m; the
crystals' 70 m hint is the game's actor-lighting reach, not a pool). The engine caps it again at
`world_radius_max_m`.

| census | class | gate (who answers) |
|---|---|---|
| vents, cells, snow bumpers, citadel devices, other emitting entities | strong | `alive` (GOAL) |
| eco clusters (`eco`: yellow, blue, red, green; 0.8 m above the root, the collision sphere the particles spawn at, collectables.gc:115; 5 m) | weak x 40 | `alive` (GOAL) |
| race rings | weak | `alive` (GOAL) |
| village1 hut lamps (`hutlamp`) | lamp | `slot` 5 (flames) |
| crates holding blue/yellow/red eco (the contents' `bake` entry in `contents_emit`: as an eco cluster, weak x 40, 5 m, 0.8 m above the root) | weak x 40 | `crate` (GOAL: `special-contents-die`, i.e. broken with contents not all collected) |
| crates holding a cell | weak | `crate` (GOAL: `special-contents-die`) |
| precursor orbs and crates of orbs | not baked | `emits: false` (design decision 2026-09-26) |
| cave crystals | strong | `crystal` (GOAL: lit state, level = emissive colour) |
| dark eco crystals (purple 0.42, 0.08, 1: the crystal's own texture colour, which the flash brightens; 2.4 m above the root; 8 m) | glow x 6 | `dark-crystal` (GOAL: the model's `color-emissive`: dark while idle and between flashes, 100% on each 0.1 s countdown flash; the explosion's starflash, 100% fading over 0.18 s) |
| dark eco boxes (`crate-darkeco-cluster`) | not baked | `emits: false` (no emissive in code, not measured as glowing) |
| props (`prop_lights.json`) | glow x gain | `always` |
| eggtops, oracle eyes, energy hub, robot-boss shield / sage cages | strong | `task-open` / `task-done` (GOAL) |
| lava meshes, dark eco pools, flame pots, miner candle | strong | `always` |
| part spawners (lightning textures: arc) | particle x group strength / arc | `always` or `hour` (engine, hour from GOAL) |
| statics on night / flame slots (lamps, torches) | lamp x min(1, energy / ref) | `slot` (engine: this frame's weight of the cluster's own E source) |
| other statics (lit trim, lava floors) | strong x min(1, energy / ref) | `slot`, or `always` without a source |

## Inventory: what looks like a light

```sh
uv run --no-sync python -m sandover.lights inventory [--levels ...]
```

Writes `$SANDOVER_DATA/lights/inventory/inventory.md` and `.json` (a subfolder: the bake reads
every `lights/*.json` as a level). One row per level and census type (entities, statics, part
spawners, props) with its count, how many are in the baked table, and the bake's reason for the
rest; plus background textures whose name reads as a light but that have no baked E, with their
cluster count and the median distance to the nearest table light. The classification (add /
already lit / should not light) lives in `kb/findings/2026-09-27-light-inventory.md`.

## Spots: where a world light shows

```sh
uv run --no-sync python -m sandover.lights spots --yaml ../shots/world-lights.yaml
```

About a minute for every level. For each strong or lamp light in the baked table whose gate is
open at the shot hour (night on levels with night slots, else noon; statics by their E source's
weight, as `unbake_emissive_weights` gives it), the level vertices within its radius get the
light the engine would add (the tile-light model with the `default` preset) and their baked
light (the vertex palette at that hour). Score = added luma / (baked luma + 0.05), summed over the
lit vertices: a strong light over dark geometry ranks first. Each ranked light gets a camera 3-5 m
from the lit patch, turned so the emitter sits 6 degrees outside the frame (its sprite stays off
screen), clear of geometry and with an unobstructed line to the patch. `--yaml` writes the best
light per kind on distinct levels plus the temple-top shot; the ranking goes to
`$SANDOVER_DATA/lights/spots/ranking.json`. Crystals only glow once hit: the command prints the
entity name the capture's stage hook must attack.

## Spot check

Project a level's lights into an existing capture (the shot's `.json` has the camera):
`R @ (pos - camera_trans / 4096)`, 64° horizontal field of view. The finding shows
the jungle vent and the tower-top power cell landing on their objects.

## Eco shots and blasts (transient lights)

```sh
uv run --no-sync python -m sandover.lights eco   # rewrite game/graphics/opengl_renderer/EcoLights.inc
```

Shots are the processes whose type descends from `projectile` or `arcing-shot` in goal_src (Jak's
yellow eco shot, the final boss's green, yellow and red shots and dark eco bomb, the mother spider's
and the ram boss's projectiles); `projectile-blue` (the blue eco stream flying into Jak at a
pickup) is not a shot. A shot's groups are the `create-launch-control` groups of its own forms;
its blasts are the `part-tracker-init` groups spawned in its forms or its base types' (the yellow
fireball hit of every projectile, the launcher flash, the bomb's explosion). Each group's colour is
the alpha-weighted mean of its lit parts, max channel 1 (`eco.shot_colour`). The table is
committed; `test_eco_lights_inc_is_current` fails while it is stale.

`pc-eco-lights-push` takes four arguments (group, position vector, size x 1024 + level, id): the
Windows GOAL-to-C thunk passes only four registers, and a fifth or sixth argument reads garbage.

Runtime: `pc-eco-lights-update` (main.gc, display loop) walks the active process tree once per
frame and reports every shot by the group its launch control holds (position, collision radius,
level) and every part-tracker by its group (a flash: level 1 - age / `*pc-eco-blast-life*`, 0.15 s;
not the tracker's duration + linger, 7 s for the yellow shot's launcher and hit). Only groups the
engine knows are sent: GOAL asks `pc-eco-lights-known` once per group and caches the answer by
address (cleared when the loaded levels change), and caches the shot-type test per type; the push
itself takes no lock (one lock per frame, at the swap in `eco_end`). A shot whose
model is hidden or absent counts only while it launches particles; its model's colour-mult scales
it (the red shot's ring fade). The engine ignores groups not in the table. Nothing is sent while
the game is paused, and the renderer keeps the last list, so a rig capture (paused) shows the
light of the frame before the pause.

Engine: `world_lights::eco_*`. Each listed shot or blast is a light the bake lacks (added,
`kAddedR0M` falloff, its own soft limit), with no fade (`snap`), in `kEcoSlots` (4) tile slots
beyond the props' and dark crystals' (nearest the camera first), and as a candidate for the actor
lights (`ECO` gate, no fade), so Jak and nearby merc props take it and it can cast Jak's shadow.
The Dark Cave's half reach follows the level Jak is in (`neighbours::viewer`): Jak 1 keeps no level
per process, and the game's own sphere test for "which level is this point in" is flagged unreliable
in its own code (`level-get-target-inside`), so a shot across the darkcave border takes Jak's side.
Radius: a shot 6 x its collision radius, a blast its group's bounds, times sqrt(strength), 4-16 m
(the floor scales down below strength 1); half in the Dark Cave. Power: the group colour x
`eco_strength` (`pc-set-emissive-lights 28`, 2.5, the strong variant, chosen as the
default) x level, so a stronger setting is both brighter
and wider (the core sits at the soft limit either way);
`pc-set-emissive-lights 27 0` turns them off for an A/B; `sandover-settings-reapply` (option 37)
puts 27-28 back to the engine defaults. Rides on the tile lights and actor
lights, so the boot state stays vanilla.

## Launch lights: entity emitters follow the game's own particle launches

Every `sparticle-launch-control` `spawn` call (sparticle-launcher.gc) is published to the engine
before the particles-off return and the launcher's visibility test, so off-screen launchers and
`particles: off` shots report too: group name, origin, the launching process's entity aid and its
root quaternion (`pc-launch-lights-push`; `pc-launch-lights-end` once per frame from the display
loop; while paused nothing launches and the last list is kept, so a rig shot keeps the frame before
the pause).

Only the glow groups in `game/graphics/opengl_renderer/LaunchLights.inc` count. That table is
generated from the `launch` entries of `entity_types.json` (`sandover-lights launch`;
`test_launch_lights_inc_is_current` fails while it is stale): the eco idle glow and its pickup
stream (`group-eco-*-collect`, a tracker spawned `:to` the eco, so the light follows the stream to
Jak, the maintainer's call on 2026-10-15), the vents' active groups and the snow-bumper field. Smoke,
debris and broken-machine groups (`citb-coil` `part-off`, `citb-generator` `part-broken`) never
light. Each group carries an offset in the launcher's frame (rotated like `vector-orient-by-quat!`):
the bumper field ring 1.5 m up (the bake's `offset_m`), the vents 1 m up the column, the eco 0
(its spawn origin is already the collision sphere).

An entity-gated row (`alive`, `crate`, `task-open`, `task-done`) whose aid launched a listed group
this frame is on at that origin plus the offset. For 30 launch frames after its last launch it is
dark; after that the GOAL query answers again at the census position. That hand-back covers a
process that stopped running: `actor-pause` stops a process-drawable beyond its pause distance
(80-220 m, varying with frame time), the snow bumper idles beyond 90 m from Jak, money orbs pause
when not drawn. GOAL is asked about launch-driven rows too, so the hand-back is current. A collected
eco is dark once its stream ends (GOAL answers 0 in `pickup` and `die`). Part spawners (`always`,
`hour`) and crystals keep their own gates: spawn runs every frame and the hour mask is applied per
item inside it.

A launched row in view takes its world slot first (like a claim). Once it has a slot, the tile
tracker drops screen tiles that stand for it (`LightTracker::Params::emitters`): within 1.5 tile
widths of its direction, between 10% (at least 2 m) nearer and its radius farther, and in its hue
(normalised dot 0.9). A far emitter's tile takes its tile's nearest surface + 1 m (`kNearSlackM`),
which can be metres in front of it (the beach ledge eco, 2026-10-15). A launched emitter without a
slot keeps its tiles; launches with no entity (Jak's and enemies' effects) are not lit by this path.
More than 1024 launching entities in a frame drops the rest (logged once). `(pc-set-emissive-lights
38 0)` turns the path off for an A/B (37 restores it). The burst log's `EMCLAIMS` line reports
`launches` (entities in the list), `calls` (every spawn call that frame) and `launch_lit` (slotted
launched rows in view).

## Actor lights

The same table lights the characters: `world_lights::select_actor` picks up to four rows near
Jak for merc lighting and two shadow maps (`kb/howto/shadows.md`, step 3). It uses its own
per-class power and falloff (`1 / (1 + d² / r0²)`, r0 5 m, reach 16 m), not the tile pass's
radius, because the bake already carries each lamp's light out to where Jak stands; skips eco,
crate contents, arcs and non-fire part spawners.
