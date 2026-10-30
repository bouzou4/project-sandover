# Level facts (Jak 1)

Per-level catalogue, day/night verdicts, transition mechanics and the Dark Cave crystal
light. Engine claims pinned at `64d72d103`.
Mood slot roles come from `facts/mood.md` (mood facts). Findings notes:
`findings/2026-09-23-level-catalogue.md`, `-transitions.md`.

Status legend: **verified** = read in source at the pinned sha; **inferred** = follows from
verified code but not observed at runtime; **external** = wiki/community source only.

## 1. Catalogue

`sky` = `:sky` flag in `level-info.gc`. "Palette cycle" = the level's baked vertex-colour
slot weights change with the clock (the level calls `update-mood-palette` with a 24-hour
interp table). "Quick" = `update-mood-quick`, a fixed slot-0 snapshot, no hour dependence.
External day/night column: nothing found on the wiki or speedrun docs states per-level
presence or absence of the cycle for any level except the final boss (see §2).

| internal | in-game name | in/out | sky | palette cycle (engine) | palette-interp table | mood facts slot roles (summary) | external day/night | level-info line |
|---|---|---|---|---|---|---|---|---|
| training | Geyser Rock | out | #t | yes | village1 | 0-4,6-7 sun; 5 light | no explicit source | `level-info.gc:11` |
| village1 | Sandover Village | out | #t | yes | village1 | 0-4,6-7 sun; 5 flames | no explicit source | `:131` |
| beach | Sentinel Beach | out | #t | yes (uses `update-mood-village1`) | village1 | as village1 | no explicit source | `:275` |
| jungle | Forbidden Jungle | out | #t | yes | village1 | 0-4,6-7 sun; 5 light | no explicit source | `:328` |
| jungleb | Forbidden Jungle temple interior | in | #f | **no** (quick) | default | slot 0 only (vent on/off) | no explicit source | `:381` |
| misty | Misty Island | out | #t | yes | misty | 0-1 sun; 2-5 flames; 6-7 light | no explicit source; heavy fog (external) | `:432` |
| firecanyon | Fire Canyon | out | #t | yes | firecanyon | 0-3 sun; 4-7 lava | no explicit source | `:616` |
| village2 | Rock Village | out | #t | yes | (village2 row, mood facts) | 0-1 sun; 2-3 lightning; 4-7 flames | no explicit source | `:687` |
| sunken | Lost Precursor City | in | **#f** | **yes** (0-1 cycle via misty table) | misty | 0-1 sun; 2 light; 3 water fade; 4-7 caustics | no explicit source | `:778` |
| sunkenb | Lost Precursor City (helix/lower) | in/mixed | #t | yes (`update-mood-sunken`) | misty | as sunken | no explicit source | `:887` |
| swamp | Boggy Swamp | out | #t | yes | (mood facts) | 0-1 sun; 2-3 lightning; 4-7 flames | no explicit source; torches (external) | `:959` |
| rolling | Precursor Basin | out | #t | yes | rolling | 0-1 sun; 2-3 lightning; 4-7 light | no explicit source | `:1160` |
| ogre | Mountain Pass | out | #t | yes (3 blended contexts) | rolling | 0-3,6-7 sun; 4-5 lightning | no explicit source | `:1212` |
| village3 | Volcanic Crater | out | #t | **yes** (slots 0-3 over 24 h) | firecanyon | 0-3 sun (table overwritten), 4-6 flames, 7 lava glow | no explicit source | `:1302` |
| snow | Snowy Mountain | out | #t | yes | firecanyon | 0-3 sun; 4-6 flames; 7 light | no explicit source | `:1393` |
| maincave | Spider Cave (main) | in | #f | **no** (quick) | village1 (unused) | 0 base; 1-2 crystal fades; 3 camera-distance fade; 4-7 flames | no explicit source | `:1598` |
| darkcave | Spider Cave (dark section) | in | #f | **no** (quick) | village1 (unused) | 0 dark base; 1-7 crystal pools | no explicit source; crystals (external, §4) | `:1688` |
| robocave | Spider Cave (robot/scaffold section) | in | #f | **no** (quick) | village1 (unused) | 0 base; 4-7 flames | no explicit source | `:1740` |
| lavatube | Lava Tube | in | #f | **no** (quick) | default (unused) | 0 base; 1 light; 2 fade (never written, §4); 4-7 lava | no explicit source | `:1811` |
| citadel | Gol and Maia's Citadel | in | #f | **no** (quick) | default (unused) | 0 base; 1-7 effects | no explicit source | `:1920` |
| finalboss | Final boss (silo top) | out | #t | bespoke fade (mood facts) | village1 | bespoke 2-context fade | **external: pre-dawn fight, sunrise in ending** | `:2144` |
| intro / demo / title / halfpipe / default-level | cutscene/menu/test | n/a | #f/#f/#f/#t/#t | default or village1 | — | — | n/a | `:2216`-`:2409` |

Sources: `goal_src/jak1/engine/level/level-info.gc@64d72d103` (lines above; `:sky` and
`:mood-func` per entry); palette tables `goal_src/jak1/engine/gfx/mood/mood-tables.gc:3442-3622@64d72d103`;
`update-mood-quick` callers `mood.gc:919,1230,1286,1300,1478@64d72d103`. Status: verified,
except the in-game names and in/out column (external: jakanddaxter.fandom.com level pages,
fetched via search snippets only).

Unset `:sun-fade` defaults to 0.0; explicit values: training/village1/beach/jungle/finalboss
1.0, misty 0.25, snow 0.5 (`level-info.gc`, verified).

## 2. Day/night verdicts where sources disagree

No day/night cycle is recalled in Volcanic Crater and Spider Cave. No external source
states either way; the verdicts rest on the engine.

| level | verdict | status | reasoning |
|---|---|---|---|
| Spider Cave (maincave, darkcave, robocave) | **confirmed: no cycle** | verified | `:sky #f` and every frame uses `update-mood-quick` slot 0 with `times[0].w = 1.0`; no hour input reaches the palette (`mood.gc:1226-1303@64d72d103`). |
| Volcanic Crater (village3) | **refuted for the palette and sky; partly true for characters** | verified (code) / inferred (visibility) | `update-mood-palette` runs with `*firecanyon-palette-interp-table*`, which walks slots 3→0→1→1/2→3/2 across 24 h (`mood-tables.gc:349-380,3556`); `:sky #t`. But `update-mood-village3` overwrites `mood-lights-table` 0-3 with constants every frame, and below camera y ≈ 45 m it lerps all four to one fixed overhead dim-red light (`mood.gc:1366-1457`), so Jak's key light is effectively time-independent on the crater floor. Whether the baked slots 0-3 differ enough to be *visible* is open until the unbake palette rip. |
| Lava Tube, Citadel, Forbidden Jungle temple (jungleb) | no cycle | verified | quick + `:sky #f`. Same pattern as Spider Cave. |
| Lost Precursor City (sunken) | cycle in slots 0-1 despite being indoors with `:sky #f` | verified (code) | `update-mood-sunken` calls `update-mood-palette` with the misty table (`mood.gc:1002-1008`, `mood-tables.gc:3378`). Mood facts: its light-group output is then overwritten by depth-lerped colours. |
| Snowy Mountain, Mountain Pass | cycle | verified (code) | palette + `:sky #t`. |
| Final boss | external says fixed pre-dawn with sunrise at the end | external (TV Tropes, via search snippet) | Consistent with the bespoke time-driven 2-context fade documented in `facts/mood.md`; not reconciled in detail. |

Clock: `time-of-day-proc` is a single global process (`time-of-day.gc:20-138`); nothing in
the quick levels stops it, so the clock keeps advancing while the lighting stays fixed
(inferred).

## 3. Transitions (summary; detail in the transitions finding)

| claim | status | source |
|---|---|---|
| At most 2 levels loaded (`level0`, `level1`) plus `level-default`; `LEVEL_COUNT` = 2 | verified | `goal_src/jak1/engine/level/level-h.gc:13,202-219@64d72d103`; `common/goal_constants.h:46` |
| Loading is driven by `*load-state*` `want` slots, updated per frame by `load-state update!`; boundaries in `load-boundary-data.gc` fire `load`/`display`/`vis`/`force-vis`/`checkpt` | verified | `level.gc:1083-1186`; `load-boundary.gc:1351-1380` |
| Every frame each loaded level runs its own `mood-func` on its own mood context | verified | `time-of-day.gc:213-228` |
| Each level's background geometry gets **its own** mood's `itimes` (palette weights) | verified | `tfrag.gc:275-278` (`(-> lev mood itimes n)`) |
| Fog, sun colour, shadow, character light-groups, `sun-fade` are a single global blend of the two moods, weight = `d0/(d0+d1)` of camera-to-bsphere distances, ramped 0.0033/frame (≈5 s at 60 Hz for a full swap) | verified | `time-of-day.gc:230-280,290-340`; `cam-update.gc:245` |
| Special case: village2 mood forced when camera y > 0 near sunken | verified | `time-of-day.gc:242-249` |
| Sky drawn if any active level has `:sky #t` | verified | `time-of-day.gc:198-202` |
| Renderer: one colour/depth target, cleared once per frame; both levels' opaque buckets (5-30) complete before bucket 31 `ALPHA_TEX_LEVEL0` | verified | `game/graphics/opengl_renderer/buckets.h:6-78`; `OpenGLRenderer.cpp:1244-1353` |
| Fog colour is one global value per frame; fog distances and `itimes` travel per bucket | verified | `OpenGLRenderer.cpp:1316`; `background_common.h:7-23`; `TFragment.cpp:229-232` |

## 4. Dark Cave crystal light (darkcave)

Mechanic (external, fandom "Explore the dark cave", via search snippet): the only light
sources are large fixed crystals; hitting one with a melee attack makes it emit strong green
light for a short while, lighting the surrounding area. The player carries nothing.
<https://jakanddaxter.fandom.com/wiki/Explore_the_dark_cave>

| claim | status | source (`@64d72d103`) |
|---|---|---|
| `cavecrystal` is a world-placed actor; `'attack` event moves `cavecrystal-idle` → `cavecrystal-active` | verified | `goal_src/jak1/levels/darkcave/darkcave-obs.gc:64-68,82-91` |
| Lit time before fade = lump `'timeout`, default 8.0 s (`300 * timeout` ticks); glow ramps up in 0.067 s, settles over 0.6 s, fades linearly afterwards | verified (constants) | `darkcave-obs.gc:48-62,164-165` |
| Crystal index = actor order in the level (`actor-count-before`), 0-6; index ≥ 7 is an error; crystal 3 is the "master" that runs connections | verified | `darkcave-obs.gc:159-160`; `levels/maincave/cavecrystal-light.gc:105-107` |
| Each active crystal writes `*palette-fade-controls*` control `id+1` with its glow and position every frame | verified | `cavecrystal-light.gc:121`; `darkcave-obs.gc:121` |
| `*palette-fade-controls*` is one global 8-entry array, reset once per frame inside `update-time-of-day` (after the mood funcs read it: one-frame latency); `set-fade!` keeps the closest writer, clamps to [0, 1.993] | verified | `time-of-day-h.gc:23-36`; `time-of-day.gc:438,442-455` |
| `update-mood-darkcave`: `times[0].w = 1` (dark base); `times[s].w = control[s].fade` for s = 1..7 — **a global per-slot weight, not per-vertex distance**. Each slot is one baked light pool around one crystal | verified | `mood.gc:1246-1269` |
| A separate 20 m falloff from each lit crystal to Jak only drives `mood-lights-table` 0 amb/prt colour (lerp (0.3,0.4,0.5)→(0,1,0.5)) and the shadow direction (forced to at least 65° below the horizon) — i.e. Jak's lighting and shadow, not the background | verified | `mood.gc:1263-1284` |
| `maincave` slots 1-2 read controls 1-2, i.e. **darkcave crystals 0 and 1** light maincave geometry too; `darkcave` lists `"maincave"` in `:run-packages` and loads beside it | verified (no other writer of 1-2 exists) | `mood.gc:1233-1234`; `level-info.gc:1688-1738`; grep of `set-fade!` |
| `robocave` never reads the fade controls | verified | `mood.gc:1296-1303` |
| `lavatube` slot 2 reads control 2, which nothing writes while lavatube is loaded, so it stays 0 | inferred | `mood.gc:1473`; `set-fade!` writers are only cavecrystal (1-7) and sunken water (3) |
| Props (`cavespatula`, `caveelevator`, `caveflag`) read crystal light via `cavecrystal-light-control` distance falloff 20480-286720 units (5-70 m) | verified | `cavecrystal-light.gc:69-70,124-137`; `maincave-obs.gc:253-258` |

Design consequences:
- **Unbake**: the lit bake of slot s is a static pre-lit pool around crystal s-1. A faithful
  replacement is a real point light (green, range of the order of the 5-70 m falloff used
  for props) at `control[s].trans`, intensity = `control[s].fade`, applied to darkcave *and*
  maincave geometry. Slot 0 (dark base) becomes a near-black ambient term. The crystal
  position is already published every frame in `*palette-fade-controls*`.
- **SSAO**: slot 0 is near-black; AO over it must be suppressed or weighted by local
  lighting (multiply AO into the crystal/point-light term only, never into the base), or
  the scene goes fully black and AO noise dominates.
- **Boundaries**: because the fade array is global, a crystal lit in darkcave changes the
  palette of maincave geometry at the same time. Any replacement light must be registered
  per crystal, not per level.
