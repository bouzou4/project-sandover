# Play session how-to (Jak 1)

A debug play session with every sandover feature on, for playing rather than capturing.
The launcher is machine-specific and lives in `sandover/local/play.py` (gitignored); this page
is the committed record of what it turns on.

```sh
cd sandover/tools
uv run python ../local/play.py [--audio binaural|surround|objects|off] [--continue <name>] [--run <slug>] [--debug-text]
```

To try a hand-staged build at a given spot: `--bin <name>` (a copy under `$SANDOVER_DATA/bin`),
`SANDOVER_ROOT=<project root>` in the environment (the staged root under
`$SANDOVER_DATA/worktrees/`, for its shaders and presets), `--at X Y Z` (game units, like a manifest's
`jak_pos`) and `--face DX DZ` to put Jak there after the start warp the way a continue point does,
and `--after <form>` (repeatable) for any other GOAL form sent once after the effects.

It takes a free instance slot (highest first), runs `(mi)` headless under the compile lock if
`out/` is stale, opens goalc in its own console (an interactive REPL), boots gk in normal debug
mode (`-boot -fakeiso -debug`, HUD on) from a staged copy of the binaries, connects them, sends
the forms below and starts at `village1-hut`. The game's on-screen debug text (console lines such as
"hint detected!" and "outside of bsp", "AS: UNKNOWN ID") is turned off with
`(set! *display-debug-text* #f)`; `--debug-text` keeps it, and the debug menu has the same toggle
(PC Settings, "Display debug text"). Config and saves: `$SANDOVER_DATA/config/play`,
seeded once from the default OpenGOAL config dir. Logs: `$SANDOVER_DATA/logs/play`.

Before staging, the launcher calls `engine.ensure_fresh`: if a tracked engine file is newer than
`gk` it runs `cmake --build out/build/Release --target gk goalc -j <logical threads - 2>` under
the build lock, and refuses to launch (printing the end of the build output) if that fails.
Uncommitted engine edits that are already built in do not trigger a rebuild. `--no-rebuild`
launches the stale binary with a warning; `--bin` (a hand-staged copy) skips the check.

## Toggles

The game also applies this look by itself from `sandover.gc` in the play config dir (Options >
Sandover, `settings.md`). The launcher still sends the forms below after every warp, so they win
over a menu change until the next warp overrides it again; use the menu with `--no-effects` to
judge a menu setting on its own. `--no-effects`, `--no-hdr` and `--hdr-output off` are written
into `sandover.gc` before boot (all off, FP16 and HDR output off, HDR output off); without them
the file is left as the menu saved it. A `--no-effects` run leaves the file all off for later
runs until the menu or another flag changes it.

| Feature | Forms | Notes |
|---|---|---|
| SSAO | none: `(sandover-settings-reapply)` after the forms re-sends the menu's tier (`pc-set-ssao-preset 0`-`3`, low to ultra) | independent of the tile lights; an index past the table is ignored with an error |
| Emissive bloom | `(pc-set-bloom 0 1000000)` `(pc-set-bloom 4 1000000)` | hue-preserving clip (the default, sent explicitly) |
| Emissive tile lights | `(pc-set-emissive-lights 0 1000000)` | engine defaults = `presets/emissive.toml` default |
| Actor shadows | `(pc-set-shadows 1 1.0)` `(pc-set-shadow-option 5 300000)` `(pc-set-shadow-option 30 1000000)` | sun and lamp maps, landing disc 0.3, sky-hued shadows in sky levels |
| FP16 target | `(pc-set-hdr-target #t)` `(pc-set-tonemap 0 0)` | clamp tonemap; `(pc-set-tonemap 0 1000000)` filmic, `(pc-set-tonemap 0 2000000)` toon (knee 4, range 5, white start 6, white full 7, halo start 8, halo full 9; `sandover-preset apply tonemap toon`) |
| HDR output | `(pc-set-hdr-output 0 1000000)` | requested by default (HDR10 PQ); the engine presents HDR only when Windows HDR is on for the game's display and stays SDR otherwise; `--hdr-output scrgb\|off`, `--hdr-paper/--hdr-peak/--hdr-black` nits; see `hdr-output.md` |
| Fur shells | `(pc-set-fur 0 1000000)` | engine defaults = `presets/fur.toml` medium (size rule exponent 1.75) |
| Water | `(pc-set-water 0 1000000)` | engine defaults = `presets/water.toml` bold (revision 3: sky reflection, swell, wave noise) |
| Material shading | `(pc-set-pbr-param 0 1)` | per-class values from `presets/pbr.toml` (normal-map strength 2.0) |
| Neighbours, unified fog, haze | the forms of `sandover-preset apply vista haze` | unified fog 150 m / 0.15 / 800 m, haze 0.85/km past 200 m, misty, geyser rock, basin and rock village fog overrides; rock village and the basin loaded for real |
| Spatial audio | none: Options > Sandover > Audio (`audio.md`) | `--audio off\|binaural\|surround\|objects` passes `gk --audio-spatial`, which overrides the Output row for that session without saving it; without it the saved row applies |

The neighbour forms, in order:

```lisp
(pc-set-vista-fog "unified" 150 150 800)
(pc-set-vista-neighbour-fog "clear" -1 -1 -1)
(pc-set-vista-haze 850 200 #x8d8f80 700)
(pc-set-vista-neighbour-fog "misty" 16000 #x7a7771 -1)
(pc-set-vista-neighbour-haze "misty" 0)
(pc-set-vista-neighbour-fog "rolling" 10000 #x6a6978 -1)
(pc-set-vista-neighbour-fog "village2" 16000 #x656581 -1)
(pc-set-vista-neighbour-fog "training" 7000 #x767371 -1)
(pc-set-vista-neighbour-haze "training" 0)
(pc-set-vista-option 5 (the int (&-> *time-of-day-context* time)))
(pc-set-vista-option 12 1)
(pc-set-vista "neighbours")
```

When a feature merges, add its toggle here and in `play.py`, taking the form from
`goal_src/jak1/kernel-defs.gc` and the chosen preset (`sandover-preset apply <phase> <name>`).

## Keys and console

| Where | Key / command | Action |
|---|---|---|
| game window | F9 | snapshot: `NNNN.png` (window framebuffer, native resolution, HUD included) and `NNNN.json` in the run folder |
| game window | F10 / F8 | next / previous level (same list as `n` / `p`) |
| game window | F7 | bug report bundle in `<run folder>/bug-reports/` (`bug-report.md`); works in every build |
| console | `n` / `p`, `l <name\|#>`, `ls` | next / previous / named level, list |
| console | `t <hour>`, `a`, `> <form>` | time of day, re-apply effects, send a GOAL form |
| console | `r` | open the run folder |
| console | `q` | quit |

The console prints `PLAY: snap #N saved (<level>, hour <h>)` once the sidecar is written.

## Snapshot run folder

At launch the script creates `$SANDOVER_DATA/runs/<YYYYMMDD-HHMM>-<slug>/` (`--run <slug>`,
default `play`) and prints it. gk gets it as `SANDOVER_RUN_DIR`; the F-keys exist only when that
variable is set, so a normal gk is unchanged. Numbering continues after the highest `NNNN.png`
already in the folder. The sidecar schema is in `snapshot-run.md`.

How it works: gk queues F9/F10/F8 presses and saves the PNG on the next rendered frame. The play
script loads `sandover/tools/sandover/rig/snap.gc` over the nREPL; its process drains the queue
every frame with `(pc-snap-poll)` and prints a `SNAP:STATE` line (level, continue, Jak, camera,
fov, hour, mood weights) or `SNAP:WARP 1|-1` to gk's stdout. The script tails the log, writes
the JSON (adding the applied toggles and the build sha) and turns warps into `n`/`p`, so the
level list lives only in `play.py`.

## Re-shooting a snapshot with the rig

```sh
cd sandover/tools
uv run --no-sync python -m sandover.rig.capture from-snapshot $SANDOVER_DATA/runs/<run>/0003.json \
    [--bin <staged build>] [--name before] [--out-dir <dir>] [--no-toggles] [--preset PHASE=NAME] [--goal FORM]
```

It captures the snapshot's level (its continue point, or the level's first one if the continue
is elsewhere), camera position and rows, Jak position and hour, at the PNG's size, with Jak,
wind, particles and time-of-day effects on and the HUD hidden. The sidecar's toggles are sent
before the shot and again after the settle frames. Run it once with the old build and once with
the fix (`--bin`, `--name`) for a before/after at the same spot. Output:
`$SANDOVER_DATA/shots/from-snapshot/<name>.png` and `.json` unless `--out-dir` is given.
The rig shot is deterministic, the snapshot is not: moving actors, particles and the running
clock differ, and the rig shoots at a frozen hour. The rig's fixed camera keeps its own field of
view, so the re-shot frame shows the same spot from the same position and direction but a little
wider than the snapshot (sidecar `fov` is recorded, not yet applied).
