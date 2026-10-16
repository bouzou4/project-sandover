# Sandover settings how-to (Jak 1)

The game's own options menu has a **SANDOVER** page that turns the enhancements on and off and
saves them in a file of their own. Background and the code map: `settings-system.md`.

## Open it

In game: pause, **OPTIONS**, **SANDOVER** (on both main settings pages, with or without the
secrets entry). Rows, top to bottom:

| Row | Kind | Values | Greyed when |
|---|---|---|---|
| Ambient occlusion | on-off | | |
| Ambient occlusion quality | list | low, medium, high; high by default (below) | ambient occlusion is off |
| Emissive light (tile lights) | on-off | | ambient occlusion is off (the tile pass reads the SSAO pass's camera-space depth and normals) |
| Bloom | on-off | | |
| Actor shadows | on-off | | |
| Shadow quality | list | low, medium, high; high by default (below) | actor shadows are off |
| Materials | on-off | | |
| Fur | on-off | | |
| Water | on-off | | |
| Sun glint on water | on-off | the sun's glint on the ocean and inland water, shaded by the world sun map (`lighting.md`, "Sun glint") | |
| Neighbour levels | on-off | neighbours plus unified fog and haze | |
| Sun | on-off | the sun, moon and stars in the sky (`lighting.md`, "Sun disc", "Sky bodies") | |
| FP16 color | on-off | | |
| HDR output | list | scRGB, PQ (HDR10), off; PQ by default | FP16 is off |
| HDR calibration | page | peak, paper white, black level (below) | FP16 or HDR output is off, or the engine is not presenting HDR (Windows HDR off, SDR fallback) |
| SMAA | on-off | SMAA 1x on top of the stock MSAA setting (Graphics page); on by default | |
| Render scale | list | 100%, 77%, 67%, 50% of the display size; 100% by default | |
| Upscaler | list | bilinear, FSR 1; FSR 1 by default | render scale is 100% |
| Save bug report (F7) | button | closes the menu and saves a bug report of the frame behind it (`bug-report.md`) | |

Looks decided by a pick are not rows: the material look stays at `bold` and the tonemap at `toon`
(the file keys `material-look` and `tonemap` still read and write, and `clamp`/`filmic` stay
reachable from the REPL and the file for the HDR path tests). The MSAA sample count is the stock
Graphics row (off, 2x, 4x, ...); its default is 2x (`PC_DEFAULT_MSAA`), so with the SMAA row the
shipped antialiasing is 2x MSAA plus SMAA (`msaa2-smaa`).

### Quality rows

Each tier keeps the look and lowers the cost:

| Row | high (default, the play look) | medium | low |
|---|---|---|---|
| Ambient occlusion quality (`ssao-quality`) | preset 4 `deep`, 16 taps | preset 5 `deep-medium`, 10 taps | preset 6 `deep-low`, 6 taps |
| Shadow quality (`shadow-quality`, shadow option 40) | 2048² sun map, 512² local maps, 3x3 PCF | 1536², 384², 2x2 PCF | 1024², 256², 1 tap (the hardware's 2x2 compare) |

The SSAO presets are entries of the engine's built-in table (`presets/ssao.toml`, generated into
`SsaoPresets.inc`); they differ from `deep` only in taps. The shadow maps are reallocated when the
tier changes.

On-off rows preview live while selected (left/right) and restore on cancel. List rows apply on
confirm. Every confirm writes the file.

### HDR calibration page

Console-style calibration (`hdr-output.md` has the mapping the values feed). Each slider draws its
test pattern in the lower middle of the frame while the cursor is on the row, applies while it
moves, restores on cancel and saves on confirm:

| Row | Range | Default | Pattern while focused |
|---|---|---|---|
| Peak brightness | 400-10000 nits, 10 per step | what the display reports | a full-signal (10000 nit) square with a diamond at the chosen peak: raise until the diamond just disappears |
| Paper white | 80-400 nits | 200 | a card with a mid-grey square: left half game white through the game's own mapping, right half fixed at the desktop's white (`DESKTOP WHITE ... NITS`); the seam vanishes when they match |
| Black level | 0-1 nit, 0.002 per step | what the display reports (0 without an MHC2 profile) | eight near-black steps 2/255 apart, the first true black: raise until the second step just shows |
| Reset to detected | button | | peak and black back to the display's values, paper white to 200 |

A value that follows the display reads `(DETECTED)` on its row; under the rows the page prints what
the display reports (`DISPLAY REPORTS ... NITS`) and the instruction for the focused pattern. Opening
a slider that follows the display writes the detected value into it, so confirming without moving
pins that value (it no longer follows the display); cancel restores the follow (-1).
The page is greyed on the SANDOVER page unless the engine presents HDR. Opened from the REPL while
the output is SDR, it works and says `HDR OUTPUT IS NOT RUNNING: SDR PREVIEW`: the patterns are then
drawn in SDR (peak scaled so 10000 nits is white, the others through the SDR path), which is what a
screenshot shows; the HDR frame itself cannot be screenshotted.

```lisp
(activate-progress *dproc* (progress-screen settings))
(activate-progress *dproc* (progress-screen sandover))
(activate-progress *dproc* (progress-screen sandover-calibration))
(set! (-> *progress-process* 0 option-index) 1)   ; once the page is up: 0 peak, 1 paper white, 2 black
```

In a rig shot, `progress-allowed?` is false while the rig's camera holds `*cheat-mode*` at
`camera`, and after-load forms run before the actors reset; open the menu from a process that
waits for `*rig-frozen*` and clears `*cheat-mode*` around each `activate-progress`, and close it
after the shot (`hide-progress-screen` once `*rig-busy*` is #f) or the next shot's load stalls.
One shot per boot is the reliable form.

From the REPL or the rig (a level must be loaded and Jak spawned):

```lisp
(activate-progress *dproc* (progress-screen settings))   ; the options page
(activate-progress *dproc* (progress-screen sandover))   ; then push the Sandover page
(set! (-> *progress-process* 0 option-index) 18)          ; cursor to the last row (scrolls)
```

A goalc compiled from a checkout without this page does not know the enum value; use
`(the-as progress-screen 67)` there (`settings` is 3).

In a rig shot, send these as after-load forms: the shot skips its own pause while a progress menu
is open (`rig-sequence`), since that pause sends the menu away.

## The file

`<config>/OpenGOAL/jak1/settings/sandover.gc`, next to the stock `pc-settings.gc`. `<config>` is
the user's config folder, or the `gk --config-path` folder: rig slots use
`$SANDOVER_DATA/config/slot<N>`, the play launcher `$SANDOVER_DATA/config/play`.

```lisp
(sandover 3
  (ssao? #t)
  (tile-lights? #t)
  (bloom? #t)
  (shadows? #t)
  (materials? #t)
  (material-look bold)
  (fur? #t)
  (water? #t)
  (neighbours? #t)
  (fp16-target? #t)
  (tonemap toon)
  (hdr-output pq)
  (paper-white-nits 200)
  (hdr-peak-nits -1)
  (hdr-black-nits -1.000)
  (antialias msaa)
  (render-scale 100)
  (upscaler fsr)
  (ssao-quality high)
  (shadow-quality high)
  (smaa? #t)
  (sun? #t)
  (glint? #t)
  )
```

- Never add a key to `pc-settings.gc`: an unmodified OpenGOAL resets every setting on the first
  unknown key. Upstream ignores `sandover.gc`, so switching builds loses nothing.
- The integer after `sandover` is the schema version (`SANDOVER_SETTINGS_VERSION`). The reader
  skips unknown keys, so an older build reads a newer file; missing keys keep their default.
  Version 2 made PQ (HDR10) the HDR output default: a version 1 file's `(hdr-output scrgb)`, the
  old untouched default, reads as `pq`. Version 3 moved the SMAA choice to its own key: an older
  file's `(antialias smaa)` or `(antialias msaa2-smaa)` reads with `smaa?` on (the mode itself is
  kept until the SMAA row is confirmed), and `sun?` takes the file's `neighbours?`, which used to
  switch the disc.
- Read at boot after `pc-settings.gc` (`initialize` in `pc/pckernel.gc`). A missing file is
  written with the defaults, and that first Sandover boot also sets the stock MSAA to
  Sandover's 2x (`sandover-first-boot!`, saved to `pc-settings.gc`) over the `(msaa 1)` a stock
  OpenGOAL saves; after that the saved value is the user's; an unreadable one falls back to them. Written on every menu confirm
  and every auto-save (`commit-to-file`), never before the first read.
- Defaults are the play build's look: everything on, both quality rows high, SMAA on over the
  stock MSAA setting (2x by default, so `msaa2-smaa`), bold, toon, HDR output requested (PQ/HDR10;
  the engine presents HDR only when Windows HDR is on, see `hdr-output.md`), paper white 200,
  peak and black from the display (`-1`).
- `paper-white-nits` is any integer, clamped to 80-400 (older files hold 150, 200, 250 or 300 and
  read unchanged). `hdr-peak-nits` is clamped to 400-10000 and `hdr-black-nits` capped at 1; a
  negative value follows what the display reports.

Tooling (`sandover/tools`):

```sh
uv run sandover-settings check <file>          # problems the game would skip or reject
uv run sandover-settings show <file>           # values after defaults
uv run sandover-settings seed-off <config dir> # all off: the game pushes nothing at boot
```

The rig seeds an all-off file into every slot before booting (`rig/capture.py`), so rig and
parity boots stay vanilla and only the forms a run sends are in effect.

## Antialiasing

The menu has two independent choices: the stock MSAA sample count (`gfx-msaa`, Graphics page) and
the SMAA row (`smaa?`). The engine mode (`pc-set-smaa 0 <mode>`) follows from both while the file's
`antialias` is `msaa` (0, the default). Defaults: SMAA row on, MSAA 2x, so mode 2 (`msaa2-smaa`):

| SMAA row | MSAA setting | Mode | Scene samples | SMAA 1x |
|---|---|---|---|---|
| off | any | 0 `msaa` | the MSAA setting | no |
| on | off | 1 `smaa` | 1 | yes |
| on | 2x | 2 `msaa2-smaa` | 2 | yes, on the resolve |
| on | 4x and up | 4 | the MSAA setting | yes, on the resolve |

`antialias` set to `smaa`, `msaa2-smaa` or `off` in the file, the REPL or a rig form (3 is the
no-antialiasing reference) overrides the two rows until the SMAA row is next confirmed, which sets
it back to `msaa`.

The override is applied where the frame's sample count is chosen (`pipelines/opengl.cpp`), so it
covers rig screenshots, whose MSAA otherwise comes from the manifest. SMAA (`Smaa.cpp`, the
reference `shaders/SMAA.hlsl`) runs after the sprite bucket and bloom, before the debug, HUD text
and subtitle buckets, on the display-encoded scene buffer: colour edges on a perceptual scale
(identity to 1.0, log-compressed FP16 headroom above it), blend weights from the reference area
and search textures, and the neighbourhood blend written back to the scene (RGB only). The
tonemap and the HDR output encode (PQ or scRGB) run after it in the final pass, so it sees the
same encoded signal with the FP16 target on or off and with HDR output on or off. HUD sprites
drawn in the sprite bucket are antialiased with the scene; text is not. `pc-set-smaa` 1 sets the
edge threshold (default 0.1), 2 switches to luma edges, 3 shows the edges (1) or blend weights (2).

## Render scale

`render-scale` (100, 77, 67, 50: percent of the display size; `pc-set-render-scale 0`) renders
the scene smaller than it is shown and `upscaler` (`bilinear`, `fsr`; `pc-set-render-scale 1`)
brings it back up. At 100 nothing changes and nothing extra runs. Below it, the order is: scene
and every scene-space pass (SSAO, tile lights, shadows, bloom, sprites with the 2D HUD and the
3D HUD icons) at the render size, SMAA there when it is on, then the upscale (`Upscaler.cpp`) to
the display size, then the `debug`, `debug-no-zbuf` and `subtitle` buckets (HUD and menu text,
letterbox, blackout, subtitles) at the display size, then the final pass (tonemap, HDR output
encode) as before. The size is applied where the frame's sizes are chosen (`pipelines/opengl.cpp`):
the display size is the game resolution the game asked for, so it also covers rig screenshots,
which then read the display-size image after the text buckets and before the HDR encode. Not with
stereo or XR.

`fsr` is AMD FidelityFX FSR 1 from `shaders/ffx` (the published `ffx_a.h` and `ffx_fsr1.h`,
unmodified; constants from AMD's own `FsrEasuCon`/`FsrRcasCon`, compiled for the CPU): EASU to
the display size, then RCAS (`pc-set-render-scale 2`, sharpness in stops, default 0.25; 0 is the
sharpest). Both run on the display-encoded scene buffer that SMAA also uses, which is perceptual
already; with the FP16 target the values above 1.0 are divided by 8 for the two filters and
multiplied back, so the headroom reaches the tonemap and the HDR output encode unclipped. If the
FSR programs fail to build (a GL 4.1 context), the upscale is bilinear.

## How it reaches the engine

`sandover-settings-update` (`pc/pckernel.gc`) runs once per frame from `main.gc`. It compares
the effective settings (a list whose parent switch is off counts as the boot state) with what it
last pushed (`*sandover-applied*`, which starts at the engine's boot state) and sends only the
fields that differ, through the existing `pc-set-*` forms. So:

- An all-off file sends nothing; defaults send the play forms once, on the first frame.
- REPL, rig and play-launcher forms are never overwritten unless the menu value changes.
- `(sandover-settings-reapply)` forgets what was pushed; the next frame re-sends every setting
  that is not in the boot state (useful after REPL experiments).

Turning a feature back on re-sends its preset, not what was there before it went off:

- Materials on (or a look change) sends `pc-set-pbr-param 11`, which reloads `presets/pbr.toml`
  and resets every PBR scalar, wiping values tuned from the REPL.
- Ambient occlusion on, or a quality change while on, sends `pc-set-ssao-preset 6 - quality`
  (4 deep at high) and the emissive mask, replacing any REPL-tuned SSAO parameter.
- Actor shadows on sends `pc-set-shadows 1 1.0` and the 0.3 landing disc; a shadow quality change
  sends `pc-set-shadow-option 40 <tier>` (a row whose switch is off counts as high).
- Sun sends `pc-set-sun-option 10` on its own; neighbour levels no longer switch the disc.
- Bloom on sends enable and the hue clip; other switches send only their enable.

Presets stay in charge of the scalars: the menu exposes switches, quality tiers and the
technical choices (FP16, HDR output format, SMAA, render scale, upscaler) only. A look
change reloads `presets/pbr.toml` (`pc-set-pbr-param 11`), which resets every PBR scalar, so a
menu slider over a preset scalar would be wiped by the next look change.

## Add a setting

1. Field in `sandover-settings` (`pc/pckernel-impl.gc`): `symbol` for on-off, `int32` index for
   a list. Default in `sandover-settings-reset!`. Bump `SANDOVER_SETTINGS_VERSION` only if an
   old value must be migrated (then migrate after `dosettings` in `sandover-settings-read`).
2. Key in `sandover-settings-read-key` and `sandover-settings-write` (`pc/pckernel.gc`); lists
   are written as names, not indices.
3. Boot state in `sandover-settings-reapply`, the effective rule (if it depends on another
   switch) in `sandover-effective!`, and the push in `sandover-settings-update`.
4. Row in `*sandover-options*` (`pc/progress-pc.gc`) and its field in `sandover-option-pointer`;
   a list row also needs a `def-progress-carousell` and a case in `sandover-option-list`. Rows
   bind by name, so order is free. A page shows 7 rows and scrolls past that; a sub-page needs a
   `progress-screen` entry (and a larger `*options-remap*`, `progress-static.gc`), a remap in
   `init-game-options`, the draw dispatch and `can-go-back?` (`progress.gc`).
5. Strings in three places: `text-h.gc` and `decompiler/config/jak1/all-types.gc` (Sandover
   block from `#x5a00`, between the `GAME-TEXT-ID` markers) and
   `game/assets/jak1/text/game_custom_text_en-US.json` (other languages fall back to English).
6. Python mirror: `KEYS`, `DEFAULTS` and `ALL_OFF` in `sandover/tools/sandover/settings.py`.
   `tests/test_settings.py` fails when the GOAL reader, writer, defaults or strings drift from it.
7. `(mi)` rebuilds the code and the text.
