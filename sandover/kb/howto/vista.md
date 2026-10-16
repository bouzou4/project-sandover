# Vista and neighbour fidelity how-to (Jak 1)

The vista renderer draws the static background (tfrag, tie, shrub, plus tfrag trans/dirt/ice/water
trees) of levels that the loader keeps resident but the game does not draw. Everything is off by
default and gated to Jak 1. Findings: `kb/findings/2026-09-23-vista-render.md` (single level),
`kb/findings/2026-09-23-vista-fog.md` (fog policies),
`kb/findings/2026-09-24-neighbour-fidelity.md` (this mode).

## Modes

| GOAL form | effect |
|---|---|
| `(pc-set-vista "village2")` | one extra level, drawn with the viewer's palette weights |
| `(pc-set-vista "neighbours")` | every level adjacent to the game's loaded levels stays resident and is drawn, each with its own palette weights |
| `(pc-set-vista "none")` | off; the extra levels are evicted after 180 idle frames |

Neighbour mode never touches GOAL's level slots: the extra names go only to the C++ loader
(`gl_set_levels` in `game/graphics/pipelines/opengl.cpp`), whose `max_levels` is raised to the
resident count + 1 while the mode is on. Adjacency is the vista adjacency table (`kb/facts/vista.md`,
`Neighbours.cpp` `kAdjacency`): the `level-info` continue pairs and `load-boundary` pairs,
title excluded. A level the game loads but does not display (for example beach at
`village1-hut`, `:disp1 #f`) is drawn by the vista too.

## Indoor levels

Neighbour mode loads and draws neighbours only while the player is in an outdoor level: the
`[outdoor] levels` list in `sandover/presets/vista.toml`, mirrored by `kOutdoor` in `Neighbours.cpp`
(`test_presets` checks the two agree). Each frame `vista-landmarks-update` sends
`(pc-set-vista-viewer <level>)` with `level-get-target-inside`; from any other level (boss arenas,
caves, sunken, citadel, lavatube, jungleb) the loader gets the vanilla level list, the vista draws
nothing, landmark copies are killed and the stand-ins stay. The gk log prints
`VISTA: <level> is not an outdoor vista level, neighbours off`, which `--wait-vista` reads as done.
The rule is symmetric: an indoor level is never a neighbour either (`adjacent_to` and
`standin_levels` return outdoor levels only, and a game-loaded, undisplayed indoor level is not
drawn), so village3 draws no ogre, maincave or lavatube, village2 no ogre or sunken, jungle no
jungleb; level boxes overlap by up to 323 m (`kb/facts/vista.md`).

The list is by hand because the stand-in census does not separate the cases: ogre carries
`medres-snow` and finalboss 16 lores/medres pieces, yet their adjacent levels (village2/village3,
citadel) overlap the arena; beach, rolling and snow carry none because vanilla draws their
neighbour for real.

## Neighbours kept out

Neighbours come from every level the game has loaded, so a level can be drawn where its
geometry stands inside another level's play space. Firecanyon's canyon walls stand on beach's
cliff top (5-40 m from Jak at the flut flut egg ledge, `runs/20260927-0137-play` 0002); the
play log there reads `VISTA: drawing beach firecanyon ... from village1's camera`, so the viewer
level was village1, not beach. `[[keep_out]]` in `sandover/presets/vista.toml`, mirrored by
`kKeepOut` in `Neighbours.cpp`, therefore keys on the camera: `level` is not drawn while the
camera is inside `inside`'s geometry box (the `detail_bbox_m` of `kb/facts/vista.md`), whichever
level is the viewer. The level stays resident, so village1 proper (outside beach's box) still
draws it. Measured with the vista point clouds (`$SANDOVER_DATA/vista/points`): other levels'
points inside a level's core box and more than 3 m from its own geometry. Firecanyon also holds
beach, jungle and rolling geometry inside its own box; not yet seen in a frame.

Rig limit: at this camera the ledge under Jak is not drawn in rig shots (black); the play frame
printed `outside of bsp`, and the rig's beach-start load does not reproduce the session's level
state there. Judge the rock mass, not the ground.

## Options (`pc-set-vista-option which value`)

| which | meaning |
|---|---|
| 0 | hide the viewer levels' stand-ins (0/1) |
| 1, 2 | fog mode / fog end (single-level mode; neighbour mode uses `pc-set-vista-fog`) |
| 3 | draw shrub (0/1) |
| 4 | GPU timers `vista-draw`, `vista-alpha` (0/1; lines `SSAO-TIMING name=...` in the gk log) |
| 5 | EE address of `*time-of-day-context*` `time`, for the neighbours' moods: `(pc-set-vista-option 5 (the int (&-> *time-of-day-context* time)))` |
| 6 | mood hour x 100 override (-1: live hour from option 5) |
| 7 | neighbours use their own mood (0: the viewer's palette weights, as the vista facts) |
| 8 | neighbours' alpha tfrag trees (trans, dirt, ice, water) |
| 9 | hide the viewer's `medres-<level>` merc stand-ins of drawn neighbours |
| 10 | hide `lores`/`medres` draws inside the neighbours (their own stand-ins of other places) |
| 11 | landmark actors of GOAL-loaded, undisplayed neighbours and loader-only landmarks (0/1, default 1; see below) |
| 12 | also keep resident the levels the viewer's stand-ins depict (`kStandins`; village2 from village1) (0/1, default 0) |
| 13 | unified fog is at least the game's own fog (0/1, default 1; see "Fog floor") |

Without option 5 or 6 the neighbours fall back to the viewer's palette weights.

Fog: `(pc-set-vista-fog "unified" <start m> <density 0.001/km> <scale height m>)`; the captures
use `150 150 800` (unified B of the fog experiment). ImGui: Renderer Debug -> Vista.

### Fog floor

The game fogs linearly in view depth: the mood's fog table gives start, end, max and min per
time-of-day slot, `update-mood-fog` lerps two slots by the hour (or a level function picks one:
finalboss before its movies is slot 3, 222-467 m; swamp raises the minimum with camera pitch;
citadel moves the end), and `update-math-camera` turns them into `hvdf.w + pzw * depth`,
clamped to [min, max] per vertex. The unified model replaced that value outright, and with 0.15
per km from 150 m it reaches only 3% at 400 m where village1 has 19%, snow 36%, rolling 63% and
misty 80% (misty ends at 180 m). With option 13 on (the default) every unified-fog shader
(tfrag3, tfrag3_sun, tie_wind, shrub, etie_base, etie fade, merc2, emerc) runs `atmo_combine`:
the total is max(game fog, unified fog + haze), and the game's part keeps the game's fog colour;
only the excess over it takes the neighbour tint and the haze colour. Where the game's fog is
the stronger (almost everywhere past its start) the frame is vanilla's fog; the height falloff,
neighbour densities and haze show only where they exceed it (misty's fit at 0.4-0.8 km from
jungle). Adding them on top of the game's fog instead overshot: far rocks and peaks vanished and
the land paled toward the haze colour (#302 first capture round). Neighbours use the viewer's
camera, so they take the viewer's game fog. The floor honours a neighbour's `max_fog` cap (none
of the presets set one). Offline model and A/B/C metric:
`uv run python -m sandover.vista.fog_model curves village1@12 finalboss:3@6` and `... measure`.

Per-neighbour fog: `(pc-set-vista-neighbour-fog "<level>" <density x1000> <tint #xRRGGBB, #x80 = 1.0>
<cap 0-1 x1000>)` scales the unified density and tints the viewer's fog colour for that
neighbour's draws; -1 keeps a default, `"clear"` drops all. Presets live in
`sandover/presets/vista.toml` (`sandover-preset apply vista haze`); the captures pass them with
`--preset vista=<name>` (from a worktree, print the forms and pass them as `--goal`).
Every background shader (tfrag3, tfrag3_sun, tie_wind, etie_base, etie, shrub) carries the
`atmo` term; the envmap pass (`etie`) fades to black instead of mixing in fog colour.

## Per-neighbour mood

`neighbours::MoodTable` (`Neighbours.cpp`) evaluates `update-mood-palette` (cycle levels) or
`update-mood-quick` (quick levels) for the hour from a table built into gk
(`NeighboursMood.inc`), then packs the 8 weights as `update-mood-itimes` does. Effect slots
(flames, lights, lava, caustics, lightning) stay at zero, as with `*time-of-day-effects*` off.
Each level has its own palette layout (fire canyon's slot 4 is black, village1's is dusk), so a
neighbour must never be drawn with the viewer's weights; the table is compiled in so that no
project-root layout can lose it (#186).

Drift risk: the table is a copy of `mood-tables.gc`. If the GOAL tables or `update-mood-*`
change, the C++ side silently diverges. The renderer checks itself every frame: it evaluates
the viewer level's own table and compares it with the itimes the game sent
(`viewer mood drift N/64` in the `VISTA:` log line and in ImGui). With effects off it should
read 0; a non-zero value with effects off means the table or the port is stale. Regenerate with
`uv run sandover-mood-tables` after any `mood-tables.gc` change: it writes the JSON for
lightbench and `NeighboursMood.inc` for gk, and a test fails while the committed table differs.

## Stand-ins

- Viewer background stand-ins: `Neighbours.cpp` `kStandins` (host, name, depicted levels), from
  the vista facts inventory (`$SANDOVER_DATA/vista/standins.csv`); hidden only while every depicted level
  is drawn. `vil1-medres-grass` is kept on purpose: it is also the ground skirt under the
  neighbours, and hiding it opens black gaps.
- Viewer merc stand-ins: `medres-<level>[N][-lodK]` models of drawn neighbours are skipped in
  `Merc2::handle_pc_model`.
- Horizon skirts (`v1-lores-mountains`, `bigpuff`, ...) stay.

## Capture

```sh
cd sandover/tools
uv run sandover-capture run ../shots/neighbours.yaml --parallel 2 \
  --goal-after-load '(pc-set-vista-fog "unified" 150 150 800)' \
  --goal-after-load '(pc-set-vista-option 5 (the int (&-> *time-of-day-context* time)))' \
  --goal-after-load '(pc-set-vista "neighbours")' --wait-vista \
  --out-dir $SANDOVER_DATA/shots/neighbours/nb
```

The forms must go after the load (`--goal-after-load`): a level load resets them, and a
`--goal` sent before the first shot leaves the shot with the vanilla stand-ins. The loader
brings the neighbours in one at a time after the continue point loads; `--wait-vista` holds
each shot until the `VISTA:` log line lists no level `waiting for the loader`. See
`rig-capture.md` "Vista captures".

## Landmark actors

In neighbour mode, a level that GOAL has loaded but does not display (beach at `village1-hut`)
gets display-only copies of the actors in `*vista-landmark-types*`
(`goal_src/jak1/pc/features/vista-landmarks.gc`): the beach windmills and the beach waterfall
particles. They draw and animate with the level's own art and mood; collision, sound, events and
perm state are not created. They are killed before the level is birthed or unloaded, when option
11 or the mode goes off, and respawned after `reset-actors`. The gk log prints
`vista-landmarks: <level> spawned N`. The copies draw through `*vista-landmark-engines*` (per
slot and texture page), run from the foreground phase in `drawable.gc`, because a level's own
foreground engines only run while it is active. Adding a type and the full list of what is
skipped: `kb/findings/2026-09-24-landmark-actors.md`.

Loader-only neighbours (jungle at `village1-hut`) have no GOAL level, entity or art. For them
`*vista-static-landmarks*` lists a merc model name or a copied part group at a fixed spot: the
model is drawn from the neighbour's FR3 by name (a `pc-merc` request built in GOAL into the
merc bucket of the actor's texture page, joints spun from a small per-slot table; the request
copies what `draw-bones` would send for the actor: camera-frame lights, the envmapped effect's
opaque base and sun-tinted envmap fade, `kb/findings/2026-10-15-fix-temple-rings.md`), and
the part group spawns from a process in `*active-pool*`. An entry applies while the loader keeps its
level resident as a neighbour (`pc-vista-neighbour?`) and GOAL has no slot for it; once GOAL loads
the level, the real actor or the whitelisted copy takes over. Revision 2:
`kb/findings/2026-09-24-landmarks-followup.md`.

Merc, emerc and world sprites also take the unified fog while it is on, so distant actors and
particles fade like the background.

Captures for landmarks need particles on: `sandover/shots/landmarks.yaml`. Pin the binaries with
`--bin` when other builds are running; `staged_bin()` otherwise falls back to the newest staged
copy, which may be another branch's.
