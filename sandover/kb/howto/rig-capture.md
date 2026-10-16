# Rig capture how-to (Jak 1)

Unattended, deterministic screenshots driven by a shot manifest. One command boots up to
four game instances at once, each on its own ports, config dir and copy of the binaries,
runs a share of the shots and writes PNG + JSON sidecar pairs. Two runs of the same
manifest give near-identical images (mean FLIP about 0.0003 over the 16-shot auto set; see
`kb/findings/2026-09-23-deterministic-capture.md`). Issues #6 and #10.

Prerequisites: the build and extraction from `build-and-extract.md` (Release binaries in
`out/build/Release/bin`, extracted assets), and a build that contains the capture-mode
engine changes listed below.

## Run it

```sh
cd sandover/tools
uv run sandover-capture run ../shots/auto-v0.yaml          # 16 shots, 4 instances
uv run sandover-capture run ../shots/auto-v0.yaml --parallel 1 --only village1-hut-noon
uv run sandover-capture one --continue-point beach-start --hour 18 --out beach-dusk.png
uv run sandover-capture compare runA/ runB/ --json flip.json   # FLIP per shot
uv run sandover-capture continues                               # all 76 continue points
```

Output: `$SANDOVER_DATA/shots/<set>/<name>.png` and `<name>.json` (override with
`--out-dir`). The CLI prints `RIG:DONE <name> slot<n>` per shot and a final
`<ok>/<total> shots in <dir> in <s>s`; it exits non-zero if any shot failed.

`run` options: `--only NAME...` (manifest order is kept), `--parallel N` (default 4,
capped by free slots), `--slot N` (use only that slot), `--settle-frames N` (override).

`run`, `one` and `from-snapshot` also take:

- `--actors live|freeze`: see "Actor freeze" below. Overrides every shot's value. Defaults:
  `run` uses the manifest's `actors` and else `freeze`; `from-snapshot` uses `freeze`; `one`
  uses `live`.
- `--max-seconds SEC`: wall cap for the whole run. No shot starts after it (the one running
  finishes, each still capped at 300 s for `RIG:READY`), crashed shots are not retried, the CLI
  prints `RIG:WALL-CAP ...` and the last line lists the shots skipped; the exit code is 1.
- `--play-look`: the game's default Sandover look after every load, sent correctly (see "The
  forms trap" below) as the first after-load form.
  Since #390 that reset turns the SMAA row on, so a shot at `msaa: 4` renders 4x MSAA plus SMAA
  (mode 4): neither the shipped 2x plus SMAA nor what captures before #390 shot. Re-shoots differ
  at edges; the manifests stay at `msaa: 4` (the vanilla library keys on it).
- `--cutscenes`: let cutscenes and scripted cameras play. By default the rig ends each one as
  it starts; see "Cutscenes" below. Per shot or in `defaults`: `cutscenes: true`.
- `--while-playing wait|fail|ignore` (default `wait`) and `--max-play-wait SEC`: what to do
  while a play session runs; see "Play sessions and input" below.

Forms and waits (`run`, `one` and `from-snapshot`):

- `--preset PHASE=NAME`, `--goal FORM` (repeatable): sent once per instance, before its first
  shot, i.e. before any level is loaded. A level load resets effect state, so anything a load
  resets (vista neighbours, for one) silently falls back to vanilla. Use them only for state
  that survives loads (e.g. `(pc-set-hdr-target #t)`, `define-extern`s).
- `--goal-after-load FORM` (repeatable): sent in every shot once its level is loaded, i.e.
  again after every continue. See "Forms after the load" below.
- `--wait-log REGEX`, `--wait-vista`, `--wait-log-timeout SEC` (default 120): hold the shot
  until a gk log line matches `REGEX` and/or no vista neighbour is still loading, then run
  the settle frames. On timeout the shot is taken anyway, the CLI prints
  `RIG:WAIT-TIMEOUT <name>` and the sidecar has `post_load.wait.ok: false`.

## Manifest (`sandover/shots/*.yaml`)

```yaml
set: auto-v0
defaults:
  resolution: [2560, 1440]
  msaa: 4
  settle_frames: 60      # frames between reset and pause
  camera: from_continue  # or follow, or { pos: [x, y, z], rot: [9 floats] }
  effects: off           # *time-of-day-effects*
  wind: off              # foliage wind
  particles: off         # sprite particles
  jak: show              # or hide
  actors: freeze         # or live (see "Actor freeze"); unset: freeze for `run`
generate: { from: continue_points, continues: [village1-hut, ...], hours: { dawn: 6, noon: 12 } }
shots:                   # explicit shots, appended after the generated ones
  - { name: hut-noon-high, continue: village1-hut, hour: 12, camera: { pos: [...], rot: [...] } }
```

`generate.hours` may also be a list (`[6, 12, 18, 23]` gets the labels dawn/noon/dusk/night).
Without `continues`, every continue point in `level-info.gc` is used. Shot names must be
unique. A shot's position in the expanded list fixes its in-game clock, so keep the order
stable if you compare runs.

`camera.rot` is the 3x3 `inv-camera-rot`, row-major, exactly as `continue-point`
`:camera-rot` stores it and as lightbench's `--cam-rot` takes it.

`goals_after_load: [FORM, ...]` (or one string), `wait_vista: true`, `wait_log: REGEX` and
`wait_log_timeout: SEC` go per shot or in `defaults` (a shot's own value replaces the default).
Command-line `--goal-after-load` forms are appended to each shot's own; `--wait-log` and
`--wait-log-timeout` replace them; `--wait-vista` turns the wait on for every shot.

`render_scale: 0.666667` and `upscaler: fsr | bilinear` (per shot or in `defaults`) send
`pc-set-render-scale` after every other after-load form of the shot, so an arm's settings reset
does not undo them; the PNG stays at `resolution` (the output size) and the scene renders at
`render_scale` of it. The engine keeps the value afterwards, so set it on every shot of a run once
one shot uses it. Recorded in the sidecar as `render_scale` and `upscaler`.

`jak_pos: [x, y, z]` (game units, optional, per shot) moves Jak there with `move-to-point!`
once the continue point is ready, before actors reset; he lands during the settle frames.
Use it for spots without a continue point, e.g. the forbidden jungle temple top
(`shadows-v1.yaml`, `emissive.yaml`). Keep him off ledges and pickups: he grabs an edge
or collects a power cell within reach. The sidecar records `jak_pos`.

## Sidecar JSON

`name, continue, hour, camera` (mode), `camera_trans`, `camera_rot` (the `*math-camera*`
trans and `inv-camera-rot` at the capture frame, read as raw float bits, so they can be fed
to `lightbench --cam-trans/--cam-rot` unchanged), `effects, wind, jak, particles,
settle_frames, resolution, msaa, slot, epoch_ticks, load_ticks, sha, timestamp`.

`cutscenes` (`skip` or `allow`), `cutscenes_ended` (one entry per cutscene the rig ended, from
its `RIG:CUTSCENE` lines: `taskable <npc> <state>`, `pov-camera <anim>`, or `other` for a movie it
cannot end), `seconds` (`ready`, `shot`: wall time from the shot's first form to `RIG:READY` and
to `RIG:SHOT`).

`play_session` (`detected`, `before`, `after`, `waited_s`): whether a play session's gk was
running right before the shot (after any wait) or right after it; `detected` is true if either
was, null if the processes could not be listed. Find suspect frames with it.

`actors` (`live` or `freeze`) and, for `freeze`, `actors_held_after` (the settle frame at which
the game printed `RIG:ACTORS freeze`; null means the line never came, so do not trust the shot
as frozen). `restarted_after_hang: true` when the shot succeeded on a fresh instance after a
hang.

What the frame was made with: `env` holds `SANDOVER_MATERIALS`, `SANDOVER_PBR_MAPS`,
`SANDOVER_PBR_PRESET` and `SANDOVER_GK_PROJ_PATH` as the CLI passed them to gk (null when
unset). `materials_log.table` is every once-per-process `materials:` line of this gk (the class
table's rows per class and the CSV it loaded, the map index file, the PBR preset file and
look, any `not found`/`unreadable` fallback); `materials_log.level` is the shot level's
texture class line. These are absolute paths of the machine; sidecars stay in the data dir.

Camera check (every mode but `follow`): `camera_requested` (`trans`, `rot`: the camera the
shot asked for, printed by the game as `RIG:CAMREQ` next to `RIG:CAMERA`), `camera_check`
(`ok`, `dist_m`, `angle_deg`, `tol_m`, `tol_deg`) and, when the rig had to fix the camera
again after the settle frames, `camera_refixed` (`region`, `state`, `slaves`, `trans`, `rot`,
`dist_m`, `angle_deg`: what the game was drawing instead). A shot more than 0.05 m or 0.25°
off prints `RIG:CAMERA-MISMATCH <name>` and counts as failed (the PNG and sidecar stay);
`--allow-camera-drift` only reports it.

`toggles`: the forms in effect: `{phase: name}` per `--preset`, `goal` (the `--goal`
forms), `goal_after_load` (this shot's after-load forms); `from-snapshot` adds `snapshot` and
`forms`. `post_load` (held shots only): `level`, `level_loaded` (a `materials: <level>` line
was printed during this shot, i.e. the continue loaded it rather than finding it resident),
`applied` (the forms sent), and with a wait `wait: {ok, seconds, log, line, vista}` (`vista` is
the last parsed `VISTA:` line: `drawing`, `camera`, `waiting`).

HDR fields: `hdr` (true when gk wrote an EXR with the shot), and when true `hdr_tonemap`
(`TonemapMode`, `TonemapExposure`, `TonemapKnee`, `Png` from the EXR header) and `hdr_stats`
(`peak`, `mean`, `over1`, `over2` of the per-pixel max channel; percentages).

## HDR captures (FP16 target on)

The PNG is always the tonemapped 8-bit image. With `(pc-set-hdr-target #t)` gk also writes
`<name>.exr` next to it, before the PNG: the buffer the final pass tonemaps (bloom and
emissive composited, HUD included), half-float RGB, uncompressed, 1.0 = SDR white. The values are display-encoded, not linear;
generic EXR viewers show the image lighter than the game. This holds
for rig shots, F9 snapshots and F2. `sandover-capture` copies the EXR with the PNG and records
`hdr: true` in the sidecar; with the target off there is no EXR and `hdr: false`.

One shot, FP16 on:

```sh
uv run sandover-capture one --continue-point village1-hut --hour 12 --out hut.png \
  --goal "(define-extern pc-set-hdr-target (function symbol none))" \
  --goal "(pc-set-hdr-target #t)"
# -> hut.png, hut.exr, hut.json ("hdr": true)
uv run sandover-hdr stats hut.exr
uv run sandover-hdr stats hut.exr --preview hut-toon.png --curve toon --toon-knee 0.95 --toon-range 1.5
```

Format, curves and caveats: `kb/howto/hdr.md`.

## Forms after the load

A shot that has after-load forms or a wait sets `*rig-hold*` before `rig-shot`. The sequence
then stops after `RIG:READY` and the camera fix, printing `RIG:HOLD <continue>`; the CLI:

1. makes sure the level's load line is in the log: the renderer prints
   `materials: <level> <n> textures, ...` (`pbr_classify_level`) from the loader thread before
   the level can go active, the same line `local/play.py` waits for before it applies its forms.
   `RIG:READY` already implies it; the level name comes from `level-info.gc`;
2. sends the after-load forms and `(format 0 "RIG:APPLIED~%")` in one nREPL message and waits
   for `RIG:APPLIED` (a compile error fails the shot instead of shooting vanilla);
3. tails `$SANDOVER_DATA/logs/slot<n>/gk.out.log` for `--wait-log` / `--wait-vista`;
4. sets `*rig-hold*` back to `#f`; the sequence prints `RIG:RELEASED` and goes on with the
   actor reset, clocks, staging and settle frames, so the image stays deterministic.

The game keeps running while held (Jak lands, actors move); step 4 of "What a shot does" resets
them afterwards. Every held shot re-sends its forms, so each continue that loads a new level
gets them again.

### The forms trap: reapply, then update, then params

`(sandover-settings-reapply)` does not push anything: it only marks every Sandover setting as
not yet sent. The next frame's `sandover-settings-update` (`main.gc`, once per frame) then
re-sends the presets (SSAO, bloom, shadows, PBR look, ...) and overwrites any parameter form
that ran before it. All after-load forms (manifest ones first, then each `--goal-after-load`)
go in one nREPL message and run before that frame, so

```sh
--goal-after-load "(sandover-settings-reset! (-> *pc-settings* sandover)) (sandover-settings-reapply)" \
--goal-after-load "(pc-set-pbr-param 3 500000)"     # WRONG: the next frame's update overwrites it
```

shoots the preset value, not yours. Correct: push the reset yourself before the params, in one
string, in this order:

```sh
--goal-after-load "(sandover-settings-reset! (-> *pc-settings* sandover)) (sandover-settings-reapply) (sandover-settings-update) (pc-set-pbr-param 3 500000)"
```

`--play-look` sends exactly the first three forms as the first after-load form, so
`--play-look --goal-after-load "(pc-set-pbr-param 3 500000)"` is the same thing. The other
correct way is an explicit look list in the manifest that never calls reapply.

## Vanilla library (never re-shoot a vanilla frame)

Frames shot with the vanilla look are indexed by camera so nobody pays for the same one twice.

```sh
uv run sandover-vanilla index [--since COMMIT|ISO]   # scan rounds/, runs/, shots/ -> $SANDOVER_DATA/vanilla/index.json
uv run sandover-vanilla find <sidecar.json>          # PNG, EXR, JSON and bin of the matching frame, else exit 1
```

A frame is indexed only if its own sidecar says it was vanilla: `toggles` holds nothing but
`snapshot`/`cutscenes` (no forms, presets, play look), no applied after-load forms, no
`SANDOVER_MATERIALS`/`PBR` env, no render scale. Folder names (`vanilla/`, `*-vanilla`), manifest
"Original game" arms and `vanilla` flags are hints only: a labelled frame that fails the check is
logged as `RIG:VANILLA-MISMATCH` and left out, and so is a frame without a sidecar.
The fingerprint is continue point, hour (0.01), camera position (0.05 m), rotation (0.25 deg),
resolution, scene flags (effects, wind, particles, Jak, MSAA) and actors/settle frames. The index
entry names the PNG, EXR, JSON and the bin (`sha`, project root); the newest capture of a
fingerprint is the one served. `index` prints the unique frames, the cameras shot more than once and
the rounds that did it. `--since` (a commit or ISO time, kept in `vanilla/since.txt`) drops captures
older than that point, for when the rig changed what a vanilla frame shows.

`run`, `one` and `from-snapshot` look a vanilla-look shot up before launching anything (no
`--preset`/`--goal`/`--goal-after-load`/`--play-look`/render scale/HDR form). On a hit at the same
resolution, within half the camera tolerance, the PNG and EXR are copied to the output folder and the
sidecar copied with `vanilla_reuse` set; the CLI prints `RIG:VANILLA-REUSE <path>`, and with every
shot reused gk never starts. Reuse is off with `--force-vanilla`, with `--bin`, with any
`SANDOVER_MATERIALS`/`PBR` env, and in paused-frame A/B: a check of a new build must shoot. The
library files are never written; a run into the folder that holds the original leaves it alone.
Every vanilla frame the rig does shoot is registered (`RIG:VANILLA-INDEXED`, under the
`vanilla-index` lock). Rebuild with `index` after frames arrive by other routes.

## Paused-frame A/B (perf-opt parity)

```sh
uv run sandover-capture run sandover/shots/perf.yaml --bin perf-base --only open-sea-low temple-top \
  --goal-after-load "(sandover-settings-reset! (-> *pc-settings* sandover))" \
  --goal-after-load "(sandover-settings-reapply)" \
  --ab-mask 0xffffffff 0xffffefff --parallel 1 --out-dir $SANDOVER_DATA/shots/ab-water-bit12
```

Each shot settles and pauses as usual, then renders the paused frame with perf-opt mask A
(`<out>/A/<shot>.png` + `.exr`), switches to mask B and renders the same paused frame again
(`<out>/B/`), then restores A. Each frame is taken `RIG_AB_SETTLE` (48) frames after its mask is
set. Game time does not advance, so A and B differ only by what the mask changes; at the end the
run writes `pixdiff` of A against B to `<out>/pixdiff.json`. `--ab-mask A A` measures the floor
(should be 0). The wave clock is pinned at 100 s (water option 90; `--ab-water-clock SEC`,
negative for live) because the waves run on wall-clock time. State that evolves per rendered
frame, not per game frame, still moves while paused: the tile-light hysteresis and fades
(bits 0-3) settle during the 48 frames on a still image, and anything else with its own clock
must be pinned or listed here.

## Vista captures

Vista neighbours (`(pc-set-vista "neighbours")`) are reset by the level load, so a `--goal`
never reaches the shot: the B candidate renders the vanilla stand-ins with no error. Send
them after the load and wait for the loader:

```sh
uv run sandover-capture run ../shots/neighbours.yaml --only temple-top-dusk \
  --goal-after-load '(pc-set-vista "neighbours")' --wait-vista
```

Add the fog and mood forms (or the forms of `sandover-preset apply vista <name>`) as more
`--goal-after-load`s. `--wait-vista` reads the renderer's line

```
VISTA: drawing <levels> from <level>'s camera; waiting for the loader: <levels> | hour ... | hide '...'
```

and releases the shot at the first one without a `waiting for the loader:` part. The
renderer logs the line only when it changes (at most once per 60 frames), so when no new line
arrives for 8 s and the last one was already complete (the neighbours stayed resident from the
previous shot), the wait ends too. Neighbours stream in over tens of seconds; if the loader has
not finished after `--wait-log-timeout` (120 s) the shot is taken anyway and marked. With
neighbour mode off, or when the game draws every level itself (`nothing to draw`), no `VISTA:
drawing` line exists: use `--wait-vista` only with the vista forms. Keep `settle_frames` as the
manifest has it; the settle frames run after the wait.

## What a shot does

`rig.gc` (sent over the nREPL once per instance) runs each shot as one in-game process, all
steps counted in frames:

| # | Step | Why |
|---|---|---|
| 1 | move all game clocks to the shot's epoch, reseed | start from the same clock every run |
| 2 | spawn the cutscene watcher (see "Cutscenes"), `(start 'play <continue>)`, wait for `rig-ready?`, print `RIG:READY` | level status is not a readiness signal under `-debug` |
| 3 | fix the camera (`debug-set-camera-pos-rot!` -> `cam-fixed`); with `*rig-hold*`, print `RIG:HOLD` and wait until it is cleared; fix the camera again | the follow camera depends on history; after-load forms and log waits; a camera region can take the camera over during the hold |
| 4 | kill all particles, `reset-actors 'life`, stop actor births for 2 frames | drop state that depends on load timing |
| 5 | clocks to epoch + 600 s; reseed `rand-vu` and `*random-generator*`; zero wind forcing, cloud scroll, particle clock; actor pause distance to its limit | same inputs for every run |
| 6 | particles on/off, actor births back on, hour set and frozen, effects on/off (off also zeroes the effect palette weights), wind on/off, HUD hidden, Jak hidden if asked | staging |
| 7 | one frame with `*pc-wind-reset*`: the renderer restarts every wind spring from rest | the wind state lives in C++ |
| 8 | 10 frames, then restart water ripple phases; `settle_frames` more frames (with `actors: freeze`, the actor groups are held after the first 20 of them, see "Actor freeze"); stage hook; if `*math-camera*` is more than 1 cm or 0.0005 per rotation element off the shot's camera, print `RIG:CAMFIX`, fix it again and run 30 more frames | palette, HUD and new actors settle; a camera region Jak entered after step 3 switched the camera master away from `cam-fixed` (#304) |
| 9 | (frozen: HUD hidden again), pause, 4 frames, print `RIG:CAMERA`, `RIG:CAMREQ` and `RIG:STAGED`, `pc-screen-shot`, 4 frames, `RIG:SHOT`, release a freeze (`RIG:ACTORS live`), unpause | screenshot from a paused, stable frame |

## Actor freeze (`actors: freeze`)

Respawned actors run during the settle frames: enemies wander, traps swing, crates fall and
Jak can be hit, so the arms of a round differ (a jungle log trap once filled the frame). With
`actors: freeze` the rig lets them run `min(settle_frames, 20)` settle frames (they pose and
land), prints `RIG:ACTORS freeze after=<n>`, then holds them with the engine's own mechanism:
a `process-mask` setting owned by the rig's sequence process,
`(set-setting! 'process-mask 'set 0.0 (process-mask enemy platform projectile death crate ambient))`,
the same call `target-death` makes when Jak dies, plus `crate` and `ambient`. `apply-settings`
copies the settings mask into `(-> *kernel-context* prevent-from-run)` every frame and
`execute-process-tree` skips any process whose mask shares a bit with it. What each bit holds:

| Bit | Set on (`init-from-entity!`) |
|---|---|
| `enemy` | every nav-enemy, seagulls, `logtrap` and other hazards, `dark-crystal` |
| `platform` | `plat`, `plat-eco`, `rigid-body` objects, jungle platforms |
| `projectile` | projectiles (`projectiles.gc`) |
| `death` | `misty-conveyor` only |
| `crate` | every crate |
| `ambient` | `part-spawner`, windmills and sails, `towertop`, gears, `harvester` |

Children (part trackers, projectiles fired by an enemy) inherit the parent's bits and hold too.

Stays live: Jak (`target`), Daxter (`sidekick`), cameras, collectables (orbs, cells, eco
pickups: `collectable`), actors with no group bit (`water-anim`, `water-vol`, eco vents), and
the particle system. Part spawners are exempted: the rig
clears their `ambient` bit while frozen and sets it back after, so torches, waterfall spray and
other emitters keep launching (`particles:` still decides whether particles run at all).
Rendering is unaffected: actors are drawn by the foreground draw engine's connections, not by
running the process, and their bones hold the last `ja-post` (as in the pause menu), so a held
actor keeps its pose. Water ripples keep moving: the waveform phase advances in
`ripple-create-wave-table` from the display frame counter for every drawn ripple mesh, not in
the actor's code.

While frozen, actor births and kills are off (`*spawn-actors*` #f; a newborn actor would never
pose) and Jak has the debug menu's `invulnerable` state flag, set again every frame, so a held
hazard touching him sends an attack that `target-attacked` ignores: no damage, no hit effect, no
HUD. The HUD is hidden once more before the pause. Not covered: `attack-invinc` hazards (dark
eco and lava pits, endless fall, `melt` surfaces) still kill; no continue point stands in one.

At the end of the shot the setting is removed, births come back, the part spawners get their
bit back and Jak's flag is cleared unless he had it before. The setting belongs to the rig's
process, so if the sequence dies the kernel's `process-disconnect` drops it, and the next
`rig-sequence` releases what is left first.

Use `actors: live` for shots that need actors running after the first 20 settle frames: dark
crystals that flash after a hook attack (`dark-crystal` is an `enemy`: the attack's event
handler runs, but the lit state's code would wait for the release), a crate that must finish
landing, geysers, lightning, anything an enemy or platform drives, a stage hook whose effect
plays out in an actor's code. Eco vents and water surfaces animate in both modes.

Capture mode (`(rig-mode #t)`) also sets `*fixed-frame-step*` (one frame of game time per
rendered frame, whatever the wall clock does), clears `*display-sha*` and sets
`*cheat-mode*` to `'camera` (no PAUSE banner).

## Cutscenes

Some continue points start a cutscene on entry in the rig's fresh game state (every task open).
The citadel ones play the green sage's introduction as soon as Jak is south of z -18821530
(`green-sagecage` idle, `citadel-sages.gc`): `citadel-start` and `citadel-elevator` waited 26-46 s
of game time for it, and its `play-anim!` moves the continue to `citadel-elevator`, so
`citadel-plat-start` never satisfied `rig-ready?` (#378's trap). Others start while the shot is
held or settling and take the camera: at `ogre-start` the `ogreboss-cam-intro` pov-camera owned
the frame (62.8 m and 34 deg off the continue camera). A cutscene plays once per instance: the
task it closes stays closed for later shots.

Unless the shot allows them (`--cutscenes`, `cutscenes: true`), the shot's sequence spawns a
child process that runs every frame, also through movies, and ends what it finds:

| Owner | How it is ended | Same as |
|---|---|---|
| `process-taskable` in `play-anim`, `play-accept` or `play-reject` (NPC and sage movies) | its `query` decision is set to `'no` with `skip-movies?` on; the spooled anim's skip callback returns true | the PC port's cutscene skip (triangle), so the task closes and cells spawn as after a skip by hand |
| `pov-camera` in `pov-camera-playing` (level event cameras: doors, platforms, battle intros) | `notify-of-abort` and `allow-abort` set, then the `'abort` event: a string anim goes to `pov-camera-abort`, a spooled one stops at its next abort check | the game's own abort path |

Anything else that sets the movie mask (camera-tracker scripts, rides) is reported once as
`RIG:CUTSCENE <continue> other` and left to play. When a cutscene was ended and the continue no
longer matches the shot's, the sequence sets it back before `RIG:READY`. `RIG:READY` and
`RIG:SHOT` print `cuts=<n>`. Input is not needed, so this works headless.

Measured on one fresh instance each, 1280x720, `actors: freeze` (A = `--cutscenes`, B = default):

| Continue | A: wall time to the frame, camera | B |
|---|---|---|
| `citadel-plat-start` | no `RIG:READY` after 300 s, restart | 13.3 s, camera exact |
| `citadel-start` | 68.6 s (ready at 7845 ticks), camera exact | 8.7 s (90 ticks), camera exact |
| `ogre-start` | 7.0 s, the boss intro camera: 62.8 m and 34 deg off, shot failed | 6.1 s, camera exact |

Six continues without a cutscene (`finalboss-start`, `firecanyon-end`, `misty-silo`, `misty-start`,
`swamp-cave3`, `village3-farside`) gave pixel-identical frames in A and B.

The `--play-look` stop at the hold in citadel (`no /RIG:RELEASED/ after 30s`) is not a cutscene:
it still happens with the introduction ended.

## Play sessions and input

**No controller input.** Every rig gk starts with these SDL hints in its environment (an
environment value beats the game's own `SDL_SetHint`, and the play session's environment is
not touched):

| Variable | Effect |
|---|---|
| `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT=0x0000/0x0001` | allow-list of one VID/PID no pad has: every SDL joystick backend (HIDAPI, RawInput, XInput, WGI, DirectInput, GameInput) skips every pad at enumeration, so the instance opens none (no input, no rumble, no HID mode switch on the maintainer's pad) |
| `SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS=0` | backup: joystick events are dropped while the window lacks focus |
| `SDL_WINDOW_ACTIVATE_WHEN_SHOWN=0`, `..._WHEN_RAISED=0` | the rig window does not take focus from the game being played when it opens |

The pc-setting `ignore-controller-win-unfocused?` was not used: it only sets the
background-events hint, which depends on focus, and a newly shown window can take focus. With no
pad, gk turns the keyboard on, but SDL sends keys only to the focused window.

**Play-session guard.** Before the run, each instance start and each shot, the rig lists
running gk processes (`Get-CimInstance Win32_Process` on Windows, `/proc` elsewhere; one listing
serves every check within 5 s) and looks for a `--config-path` ending in `config` + `\` or `/` +
`play`, any case, quoted or not (the play launcher's config dir). It never kills or signals that
process. A gk whose command line cannot be read (an elevated one) makes the answer unknown
(`null`), not "no session"; a listing that fails during a wait counts as still playing.
`--while-playing`:

- `wait` (default): print `RIG:PLAY-WAIT <what>` once, check again every 30 s, print
  `RIG:PLAY-RESUME <what> after <s>s` when the session has ended. Waiting counts towards
  `--max-seconds`: at the wall cap the wait ends and the remaining shots are skipped as for any
  wall cap. Without `--max-seconds` it waits as long as the session lasts. `--max-play-wait
  SEC` bounds one wait; past it the instance's remaining shots are skipped (`RIG:PLAYING`).
  A wait before an instance start that ends at the wall cap starts nothing (no goalc `(mi)`, no
  window). A wait before a shot that lasts 60 s shuts the booted instance down (`RIG:PLAY-WAIT
  slot<n>: stopping the idle instance ...`); it boots again after `RIG:PLAY-RESUME`.
- `fail`: a session at the run's start captures nothing; at a later check the instance skips
  its remaining shots. Exit code 1 in both cases.
- `ignore`: capture anyway; the sidecar still records it.

A shot taken while a session ran (in `ignore` mode, or one starting mid-shot) prints
`RIG:PLAYING <shot>: a play session was running at capture time` and has
`play_session.detected: true`.

`perf.py`, `cost.py`, `fur_cost.py` and `scripts/speed_test.py` start their gk through `Game`, so
they get the no-input environment, but they have no play-session guard: check by hand before a
measurement.

## Engine changes behind capture mode

All default off; vanilla behaviour is unchanged unless a flag is set.

| File | Change |
|---|---|
| `goal_src/jak1/engine/draw/drawable.gc` | `*fixed-frame-step*`: `display-frame-start` uses time-ratio 1.0 every frame |
| `goal_src/jak1/engine/gfx/tie/tie-methods.gc` | `*pc-wind-reset*`: the wind DMA word is 2 instead of 0/1 |
| `game/graphics/opengl_renderer/background/Tie3.cpp` | wind word 2 zeroes the wind spring state (Jak 1 branch only) |
| `goal_src/jak1/engine/gfx/sprite/sparticle/sparticle-launcher.gc` | with `*particles-flag*` #f, `spawn` launches nothing (nothing in vanilla clears the flag) |
| `game/main.cpp`, `game/runtime.cpp` | `gk --port N` now takes a value and the listener binds it (was a flag, and the port was ignored) |
| `goalc/make/MakeSystem.cpp`, `goalc/make/Tools.cpp` | `OPENGOAL_MAKE_NO_WRITE=1`: `(mi)` compiles for type info only and writes nothing to `out/` |

## Instances, slots and locks (`sandover.locks`, `sandover.engine`)

- **Slots** `0..7`: slot n uses game port `8112+10n`, nREPL `8181+10n`, debug server
  `8128+10n`, config dir `$SANDOVER_DATA/config/slot<n>`, logs `$SANDOVER_DATA/logs/slot<n>`.
  A slot is free when `locks/slot<n>.lock` is absent and nothing listens on its ports.
  A lock whose holder pid (same host) is not running is taken over at once
  (`reclaimed slot<n> lock: holder pid N is not running`; the exit code is read, because
  Windows keeps an exited process openable while a handle to it is open). gk or goalc processes
  that a dead holder left listening on the slot's ports (a force-killed run) are stopped
  (`reclaimed slot<n>: holder pid N is not running; stopping its goalc.exe pid M on port P`):
  only processes named gk/goalc whose parent is the dead pid in `slot<n>.purpose` and whose
  command line is the rig's own for that slot (gk: its `--config-path config/slot<n>`; goalc:
  its `--port` and `--debug-port`). A hand-run or build goalc, a play session's gk and any
  process whose command line cannot be read are never touched. A run waits for a slot at most until its `--max-seconds` wall cap, then prints
  `RIG:SLOT-BUSY <why per slot>` and a summary with the shots `skipped (slot busy)`.
  Slot 0 is also taken while a hand-made `locks/game.lock` exists (someone running the game
  on the default ports by hand). `(lt <game port>)` connects goalc to its own gk.
  Every boot rewrites the slot's `sandover.gc` all off (`settings.seed_off`), so the game's
  in-menu Sandover defaults never apply to a rig run; only the run's forms do (`settings.md`).
- **Engine logs**: gk and goalc keep their own rotating logs (`compiler.*.log`,
  `jak1.*.log`, ten kept). `OPENGOAL_LOG_DIR` moves them out of `$SANDOVER_ROOT/log/`; the rig
  sets it to `logs/slot<n>/engine`. Rotation skips a log that another running instance holds
  open. Before both changes, a long-running goalc or gk (a play session) whose log became the
  oldest in the shared directory made every later start fail with `Failed to setup logging`
  (issue #176).
- **Binaries**: before launching, `sandover.engine.staged_bin()` copies `gk`, `goalc` and
  the DLLs to `$SANDOVER_DATA/bin/<gk build time>-<hash>/` (once per build) and runs them
  from there, so `ninja` can relink while games run. Game data and shaders are still found
  through `OPENGOAL_PROJ_PATH`. Each run then prunes old copies: it keeps the newest three,
  the one in use, and any copy a live slot lock names (slot purposes contain `bin <dir>`);
  a copy still running cannot be removed and is skipped. Manual: `sandover-capture prune
  [--keep N]`. Directories without the `.complete` marker (not made by this tool) are left alone.
- **build lock**: one ninja at a time; a running game does not block a build. Staging the
  binaries takes it briefly (if a build holds it, the newest existing copy is used).
- **compile lock**: a goalc that writes `out/` (`(mi)` when any GOAL source is newer than
  `out/jak1/iso/KERNEL.CGO`) holds it; one instance per run does that write, the others wait
  and then compile for types only (`OPENGOAL_MAKE_NO_WRITE`). When `out/` is current, all
  instances compile for types only and nothing under `out/` is touched.
- Lock files hold filelock's PID record; the purpose and time go to `<name>.purpose`. To hold
  a lock by hand, create `locks/<name>.lock` with a purpose and time and delete it after;
  tools wait for it.

```python
from sandover import locks
with locks.hold(locks.BUILD, "rebuild gk"): ...
with locks.slot(purpose="my experiment") as s: print(s.game_port, s.config_dir)
```

**GPU cost measurements must be taken solo**: run with `--parallel 1` and nothing else on
the GPU, and say so in the note that reports them.

## Parallel runs

Shots are grouped by level so each instance loads its levels once; with more instances than
levels the largest level is split. Instance starts are staggered by 2 s (binaries older than
`OPENGOAL_LOG_DIR` rotate their logs in one shared directory and simultaneous starts race). A
shot that fails is reported and the instance moves on; if an instance dies, its remaining
shots are retried once on the slot that crashed (the `--slot` given, never another) after the others finish;
if that slot is still busy the retry waits for it and logs `RIG:RETRY slot<n>: <reason>`.

A shot that gets no `RIG:READY` within 300 s (`RIG:FAIL <shot> no /RIG:READY.../ after 300s`,
usually the IOP sound-RPC deadlock on a level load, #312) leaves the instance stuck. The CLI then
kills that slot's gk and goalc (only the processes it started), prints `RIG:RESTART slot<n>
after <shot> hung`, boots a fresh pair on the same slot, retries the shot once and continues
with the rest. A second hang of the same shot fails it.

Timings, 16-shot auto set, `out/` current:

| Instances | Wall time | (mi) phase | Shots phase | CPU mean/max | GPU mean/max |
|---|---|---|---|---|---|
| 1 | 71 s | ~11 s | ~51 s | 17% / 31% | 19% / 32% |
| 4 | 57-65 s | ~35-40 s | ~15 s | 29% / 71% | 18-20% / 66-95% |
| 8 | 106 s | ~90 s | ~13 s | 41% / 100% | 18% / 100% |

The concurrent `(mi)` type compiles dominate: four of them take about 3.5x as long as one.
For 16 shots, 4 instances are the sweet spot; for large sets (hundreds of shots) more
instances pay off once the compile is amortised. A first try at 8 without the start stagger
lost one goalc to the log race and one gk mid-load; the retry path recovered all shots.

## Worktree builds

Build a worktree with the shared recipe, not a hand-written `build.py`:

```sh
cd $SANDOVER_ROOT/sandover/tools
uv run --no-sync python -m sandover.engine build <worktree> [targets...]   # default target: gk
```

It waits for a free build lane (`build`, or `build-b` with 10 GB free memory), configures only
when the worktree has no `out/build/Release/build.ninja`, then runs ninja with `-j` = logical
threads - 2, computed when the build starts. The configure command
(`python -m sandover.engine configure-cmd <worktree>` prints it) adds `-C
sandover/cmake/configure-seed.cmake` (cached try-compile results, `sandover.buildseed`; only for a
new build tree), ccache as the launcher, and the real `ninja`/`ccache` exes behind the scoop shims.
On 2026-10-15 a seeded configure took about 1 min; a plain one is an estimated 6 min on a quiet machine
and took 51 min beside another build (`kb/findings/2026-10-15-tools-build-speed.md`). Every lock
hold is logged; `python -m sandover.engine holds --min 10` lists the long ones.

gk reads its shaders from the project root, so a worktree build needs a root with the
worktree's `game/` and the main checkout's data. Stage a copy; never junctions or symlinks
(a recursive delete follows them into the main checkout).

1. Binaries: copy the worktree's `gk` and DLLs to `$SANDOVER_DATA/bin/<name>/`. If the
   worktree built only `gk`, add `goalc` and the DLLs it lacks (`compiler`, `decomp`, `Zydis`,
   ...) from the main build without overwriting the worktree's. Leave out `.complete`, so
   `prune` never touches the copy.
2. Root: copy into `$SANDOVER_DATA/worktrees/<name>-root/` (robocopy `/E /XJ`): the worktree's
   `game/` and `sandover/presets/`; the main checkout's `out/jak1/`, `iso_data/jak1/`,
   `decompiler_out/jak1/`, `custom_assets/`, `decompiler/` and `goal_src/`; and the main
   checkout's `sandover/data/lighting/`, `sandover/data/sky/` and `sandover/data/materials/`.
   Add an empty `log/`. About 4 GB.
   gk reads those three folders from `$SANDOVER_DATA/<dir>` and falls back to the project root
   only when `SANDOVER_DATA` is unset (Canopy.cpp, CanopyVol.cpp, SunDisc.cpp, Materials.cpp),
   so the copies matter only for a run without it; copying them is harmless. A root without
   them (and with `SANDOVER_DATA` set) once timed about 1.5 ms slow at every spot
   (`actor-shadows`, `tfrag`) against the main checkout; the cause is not established
   (`kb/findings/2026-10-10-opt-water.md`).
3. Set `SANDOVER_DATA` explicitly: the copied root has no `sandover/data/` of its own, and
   without the variable gk loads no PBR materials.

```sh
SANDOVER_ROOT=$SANDOVER_ROOT SANDOVER_DATA=$SANDOVER_DATA \
SANDOVER_GK_PROJ_PATH=$SANDOVER_DATA/worktrees/<name>-root \
  uv run --no-sync python -m sandover.rig.capture run <manifest> --bin <name>
```

goalc and `(mi)` still use `$SANDOVER_ROOT`, so a branch's new kernel functions need
`--goal "(define-extern ...)"` forms. Round presets that exist only on the branch: compute the
forms with the worktree's tools (`SANDOVER_ROOT=<worktree>`, `PYTHONPATH=<worktree>/sandover/tools`,
`presets.goal_forms(phase, name)`) and pass them as `--goal`. The copy holds no links; list
reparse points before deleting it anyway.

Water A/B: the wave clock follows the frame counter, which differs between runs, so two
captures of one snapshot show different crests. Pin it with `--goal "(pc-set-water 90 <us>)"`
(seconds x 1000000; negative is live) on builds that have it (#296).

`*rig-stage-hook*` (a `(function none)`, set with `--goal`) runs once after the settle frames,
when actors exist. For example, the vista fog experiment kills misty's `med-res-level`
stand-in actor with it.

## Manual use

```sh
uv run python -m sandover.rig.nrepl "(lt 8112)"
uv run python -m sandover.rig.nrepl --file sandover/rig/rig.gc
uv run python -m sandover.rig.nrepl --port 8191 '(format 0 "RIG:HELLO~%")'   # slot 1
```

The nREPL never returns results; `RIG:*` lines in the slot's `gk.out.log` are the feedback.

## Known quirks

- `run` sends only the given forms. SSAO defaults off, and with SSAO off `EmissiveLights::render`
  skips the world/tile lights. Pass the play toggles (`--goal` and `--goal-after-load`) or
  `(pc-set-ssao-preset 4)` for any emissive/tile-light shot (#301, crystals-002).
- goalc with stdin at EOF spins one core in its input loop (a never-written pipe stops its
  stdout flushing and `(e)` being honoured).
- goalc has no `--config-path`; the rig points `APPDATA` at the slot's config dir.
- `pc-screen-shot` logs `GL_INVALID_OPERATION ... The required buffer is missing` with MSAA;
  the PNG is written correctly.
- `camera: follow` is not reproducible (the follow camera depends on history).
- Camera regions: every unpaused frame the camera master checks the camera region volumes
  around Jak (`master-check-regions`) and switches state when he enters or leaves one, replacing
  the rig's `cam-fixed`. Shots with `jak_pos` far from the continue point hit this (#304). The
  rig fixes its camera again after the hold and, if it drifted, after the settle frames; a
  `camera_refixed` in the sidecar names the region and state that took over.
