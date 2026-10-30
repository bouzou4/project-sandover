# Sandover settings how-to (Jak 1)

The game's own options menu has a **SANDOVER** page that turns the enhancements on and off and
saves them in a file of their own. Background and the code map: `settings-system.md`.

## Open it

In game: pause, **OPTIONS**, **SANDOVER** (on both main settings pages, with or without the
secrets entry). Rows, top to bottom:

| Row | Kind | Values | Greyed when |
|---|---|---|---|
| Vanilla rendering | on-off | upstream's renderer: MSAA at the stock setting, no Sandover effects; off by default (below) | the engine cannot run temporal AA (it is then on in effect) |
| Ambient occlusion | on-off | | |
| Ambient occlusion quality | list | low, medium, high, ultra; medium by default (below) | ambient occlusion is off |
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
| Motion blur | list | off, weak, medium, strong; medium by default (below) | |
| Camera motion blur | list | off, vehicles, always; vehicles by default: the camera's own motion blurs too, while the player rides the zoomer or the flut flut, or always (below) | motion blur is off |
| Contact shadows | on-off | screen-space contact shadows (`shadows.md`, "Contact shadows"); off by default; on sends `pc-set-shadow-option 41` at the strong level | actor shadows are off, temporal AA is not running, or stereo or XR runs (they do not run there) |
| FP16 color | on-off | | |
| HDR output | list | scRGB, PQ (HDR10), off; PQ by default | FP16 is off |
| HDR calibration | page | peak, paper white, black level (below) | FP16 or HDR output is off, or the engine is not presenting HDR (Windows HDR off, SDR fallback) |
| Upscaler | list | TEMPORAL AA (FSR 3.1), DLSS, FSR 4 (each offered only where it can run; DLSS not in stereo or XR); FSR 3.1 by default (below). In stereo and XR one context per eye | vanilla rendering |
| Render scale | list | 100%, 77%, 67%, 59%, 50%, 33% of the display size, each with its quality name and render size; 100% by default (below). In XR: of the eye frame, with the per-eye size; 67% by default in the VR profile | neither temporal AA nor an XR session runs |
| Audio | page | Output (Auto, Stereo, 5.1, 7.1, Headphones, Spatial), Device, Speaker test (`audio.md`, "The Audio page"); Spatial applies at the next game start | never by vanilla rendering; its rows grey while `gk --audio-spatial objects` runs |
| Save bug report (F7) | button | closes the menu and saves a bug report of the frame behind it (`bug-report.md`) | |

Settled looks are not rows: the material look stays at `bold` and the tonemap at `toon`
(the file keys `material-look` and `tonemap` still read and write, and `clamp`/`filmic` stay
reachable from the REPL and the file for the HDR path tests). Every row but Vanilla rendering,
Audio and Save bug report greys while vanilla rendering is on. The stock MSAA row (Graphics page; 2x by
default, `PC_DEFAULT_MSAA`) applies to vanilla rendering only: temporal AA renders the scene at
one sample, also per eye in stereo and XR.

### Flat screen and VR profiles

Two complete sets of these rows: the flat screen profile and the VR profile. While an OpenXR
session runs (`[xr] state ... -> RUNNING`, `howto/xr.md`) the game runs the VR profile; otherwise the flat
one. The switch happens on the next frame after the session starts or ends, and pushes only what
differs, like any menu change. The page names the profile it edits at the top (FLAT SCREEN
SETTINGS or VR SETTINGS). Editing a row in one profile never changes the other: each has its own file
(below), and the stock MSAA row on the Graphics page belongs to the active profile too (the MSAA
of vanilla rendering in the headset). While a row is open for editing the profile does not switch
until it closes, so a cancel restores the value into the profile it came from.
Vanilla rendering is a row of each profile too: saved on in the VR profile, every XR boot runs
upstream's path (no upscaler, eyes at 100%), and the switch line says so
(`[PC] Sandover settings profile: vr (vanilla rendering)`).

Every row does the same thing in both profiles. Differences come only from the engine state, not
from the profile: in stereo and XR the upscaler runs once per eye (FSR 3.1 or FSR 4, each eye
its own context and history; DLSS is not offered there), contact shadows are greyed (they do not
run in stereo), the HDR output and HDR calibration rows are greyed while an XR session runs (PCVR
has no HDR output: the eye swapchains are sRGB, see `howto/xr.md`), and the render scale sizes each eye's frame: each eye is drawn at the scale times
its frame size and the upscaler reconstructs the full frame, which is cropped into the runtime's
swapchain image. The row shows the render size per eye, `67% (3009X2324 PER EYE)` on a PSVR2 at
SteamVR's default resolution; the frame size comes from the engine once the headset has drawn a
frame (`findings/2026-10-28-vr-per-eye-upscaling.md`).

VR defaults are the flat defaults with the render scale at 67%: with today's one-frame-for-both-eyes
projection a PSVR2 eye is drawn at 4491x3468 at 100% (`findings/2026-10-27-vr-bringup.md`), and 67%
is the largest entry under a 4K frame's pixel count (3009x2324, 7.0 Mpx against 8.3; 77% would be
9.2). The VR MSAA starts from the flat screen's.

REPL: `(set! *sandover-profile-force* 'vr)` holds the VR profile without a headset (`'flat` the flat
one, `#f` follows the session again); `(sandover-profile-settings 'vr)` is the VR set wherever it
is held; `(commit-to-file *pc-settings*)` writes both files.

### Quality rows

Each shadow tier keeps the look and lowers the cost:

| Row | high (default, the play look) | medium | low |
|---|---|---|---|
| Shadow quality (`shadow-quality`, shadow option 40) | 2048² sun map, 512² local maps, 3x3 PCF | 1536², 384², 2x2 PCF | 1024², 256², 1 tap (the hardware's 2x2 compare) |

The ambient occlusion tiers are GTAO budgets (`howto/ssao.md`, `findings/2026-10-18-gtao.md`); the preset index is the tier:

| `ssao-quality` | SSAO preset | GTAO |
|---|---|---|
| low | 0 `deep-low` | 1 slice x 2 steps, half resolution |
| medium (default, the play look) | 1 `deep` | 2 x 2, half resolution |
| high | 2 `deep-high` | 2 x 3, half resolution |
| ultra | 3 `deep-ultra` | 3 x 3, full resolution |

Every tier runs as saved; without the temporal AA path (MSAA, vanilla) the AO has no history and
runs a fixed noise pattern with three denoise passes. In stereo and XR each eye keeps its own AO
history. The SSAO presets are entries of the engine's built-in table
(`presets/ssao.toml`, generated into `SsaoPresets.inc`); all four share `deep`'s radius and
strength. The shadow maps are reallocated
when the tier changes.

On-off rows preview live while selected (left/right) and restore on cancel. List rows apply on
confirm. Every confirm writes the file.

### Motion blur

Per-object motion blur (`MotionBlur.h`, `findings/2026-10-19-motion-blur.md`): an actor never
blurs more than it moves on screen and the camera's motion never counts: it blurs by its motion
against the world (v_full - v_cam), scaled down to at most its on-screen motion |v_full|. The static
world stays sharp while the camera moves, Jak held by the follow camera stays sharp, a still enemy
under a pan stays sharp, a spin attack or a running enemy blurs fully. The HUD and text never blur. The row sends
`pc-set-motion-blur 0 <level>`; the engine turns the motion vectors on by itself while the level is
not off. The strength is the shutter, a fraction of the frame interval: weak 0.25, medium 0.5 (a
180 degree shutter), strong 1.0. It follows the frame rate as a camera would: at 144 fps the same
motion blurs about 2.4x less than at 60. Not in stereo or XR.

Camera motion blur (Off / Vehicles / Always, default Vehicles): while active, everything blurs by
its on-screen velocity, actors by v_full and the world by the camera's motion. Vehicles does so only
while the player rides the zoomer or the flut flut, Always everywhere. It takes the motion blur
row's strength and is greyed while that row is off. The row sends `pc-set-motion-blur 10 <mode>`;
the mounted flag is the game's own test (`collide-action racer` or `flut` on the target's root
prim, set when Jak mounts and cleared when he gets off), sent as `pc-set-motion-blur 11` when it
changes.

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
(set! (-> *progress-process* 0 option-index) 22)          ; cursor to the last row (scrolls)
(activate-progress *dproc* (progress-screen sandover-audio))   ; the Audio page
```

A goalc compiled from a checkout without this page does not know the enum value; use
`(the-as progress-screen 67)` there (`settings` is 3).

In a rig shot, send these as after-load forms: the shot skips its own pause while a progress menu
is open (`rig-sequence`), since that pause sends the menu away.

## The file

`<config>/OpenGOAL/jak1/settings/sandover.gc` (flat screen) and `sandover-vr.gc` (VR), next to the
stock `pc-settings.gc`. `<config>` is
the user's config folder, or the `gk --config-path` folder: rig slots use
`$SANDOVER_DATA/config/slot<N>`, the play launcher `$SANDOVER_DATA/config/play`.

```lisp
(sandover 4
  (vanilla-rendering? #f)
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
  (render-scale 100)
  (upscaler fsr3)
  (ssao-quality medium)
  (shadow-quality high)
  (sun? #t)
  (glint? #t)
  (contact-shadows? #f)
  (motion-blur medium)
  (camera-blur vehicles)
  (audio-output auto)
  (audio-device "")
  )
```

- `sandover-vr.gc` has the same keys plus `(msaa N)`, the VR profile's stock MSAA. `pc-settings.gc`
  always holds the flat screen's MSAA: while the VR profile runs, the commit writes the flat value
  there and the VR one into `sandover-vr.gc`. A VR file without `msaa` takes the flat screen's.
  A missing VR file is written with the VR defaults at boot (an install from before the split gets
  the VR defaults, not a copy of its flat values); an unreadable one falls back to them.
- Never add a key to `pc-settings.gc`: an unmodified OpenGOAL resets every setting on the first
  unknown key. Upstream ignores `sandover.gc`, so switching builds loses nothing.
- The integer after `sandover` is the schema version (`SANDOVER_SETTINGS_VERSION`). The reader
  skips unknown keys, so an older build reads a newer file; missing keys keep their default.
  Version 2 made PQ (HDR10) the HDR output default: a version 1 file's `(hdr-output scrgb)`, the
  old untouched default, reads as `pq`. Version 3 gave the sun its own key: `sun?` takes an older
  file's `neighbours?`, which used to switch the disc. The `antialias` and `smaa?` keys of files
  written before temporal AA became the only Sandover path are skipped, and their `upscaler`
  (`bilinear`, `fsr`) keeps the default, `fsr3`.
  Version 4 gave ambient occlusion four GTAO tiers: an older file's `high` (SAO deep) reads as
  `medium`, the tier tuned to match it, and its `medium` and `low` as `low`.
- Read at boot after `pc-settings.gc` (`initialize` in `pc/pckernel.gc`). A missing file is
  written with the defaults, and that first Sandover boot also sets the stock MSAA to
  Sandover's 2x (`sandover-first-boot!`, saved to `pc-settings.gc`) over the `(msaa 1)` a stock
  OpenGOAL saves; after that the saved value is the user's; an unreadable one falls back to them. Written on every menu confirm
  and every auto-save (`commit-to-file`), never before the first read.
- Defaults are the play build's look: vanilla rendering off, everything on, both quality rows
  high, temporal AA at 100 % (native), bold, toon, HDR output requested (PQ/HDR10;
  the engine presents HDR only when Windows HDR is on, see `hdr-output.md`), paper white 200,
  peak and black from the display (`-1`).
- `audio-output` is auto, stereo, 5.1, 7.1, headphones or spatial (read as text: `5.1` is not a
  symbol). gk also reads `audio-output` and `audio-device` from `sandover.gc` at start, before
  OpenAL Soft's first call (`main.cpp`, `snd::ParseSavedAudio`), because Spatial is OpenAL Soft's
  config and is read once per process.
  `audio-device` is OpenAL Soft's device name in quotes, cut at 127 bytes; `""` (or a bare word)
  is the Windows default. Neither is part of vanilla rendering. No version bump: an older file
  has neither key and gets Auto on the Windows default; an older build skips them.
- `paper-white-nits` is any integer, clamped to 80-400 (older files hold 150, 200, 250 or 300 and
  read unchanged). `hdr-peak-nits` is clamped to 400-10000 and `hdr-black-nits` capped at 1; a
  negative value follows what the display reports.

Tooling (`sandover/tools`):

```sh
uv run sandover-settings check <file>          # problems the game would skip or reject
uv run sandover-settings show <file>           # values after defaults
uv run sandover-settings seed-off <config dir> # vanilla rendering in both profiles: the game pushes nothing at boot
```

The rig seeds vanilla-rendering files (both profiles) into every slot before booting (`rig/capture.py`), so rig
and parity boots stay vanilla and only the forms a run sends are in effect.

## Antialiasing

Temporal AA is the only Sandover render path. The engine runs AMD FidelityFX FSR 3.1
(`findings/2026-10-18-fsr3.md`, `Fsr3Upscaler.cpp`) as antialiasing mode 1 (`pc-set-aa 0 1000000`):
the scene at one sample with a sub-pixel jitter, reconstructed from the frame history after the
sprite bucket. `upscaler` names the temporal upscaler: `fsr3` (0), `dlss` (1) or `fsr4` (2). The engine
mode is the value plus one (`pc-set-aa 0 2000000` for DLSS, `3000000` for FSR 4). The applied copy holds -1 for the engine's boot
mode, 0 (MSAA at the stock setting).

DLSS (NVIDIA DLSS super resolution, `findings/2026-10-18-dlss.md`, `DlssUpscaler.cpp`) takes FSR
3.1's place, inputs and jitter. The render scale keeps its exact percent: the engine picks the DLSS
mode whose render size range (NGX's optimal settings at the display size) contains it, the nearest
optimal size first, and the row names that mode: 100% DLAA, 77% and 67% QUALITY, 59% BALANCED, 50%
PERFORMANCE, 33% ULTRA PERFORMANCE. The menu offers DLSS only where it can run
(`(pc-get-aa-available 1)`: an NVIDIA adapter, WGL_NV_DX_interop2, a DLSS runtime NGX can load).
A file that names it elsewhere, or a later device or feature failure, runs FSR 3.1 instead.
REPL only: `pc-set-aa 6 <bits>` negates the jitter x (1) or y (2) passed to DLSS (a diagnostic,
default 0); `pc-set-aa 5` resets it with 2-4.

FSR 4 (AMD FSR 4, `findings/2026-10-22-fsr4.md`, `Fsr4Upscaler.cpp`) takes FSR 3.1's place,
inputs, jitter and sharpness on the D3D12 device (`D3d12Bridge.h`). The render scale keeps its
exact percent and FSR 3.1's names (100% NATIVE). The menu offers it only where it can run
(`(pc-get-aa-available 2)`: a D3D12 adapter behind the GL context and an FSR 4 provider in the
shipped runtime for it); the list value is its index, so the DLSS entry before it is stepped over
where DLSS cannot run (`sandover-list-skip?`). A file that names it elsewhere, or a later context
or dispatch failure, runs FSR 3.1 instead. REPL only: `pc-set-aa 7 <bits>` negates the jitter x
(1) or y (2) passed to FSR 4 (a diagnostic, default 0), reset by `pc-set-aa 5`. No settings
version bump: an older reader keeps its default (`fsr3`) for the new name.

Vanilla rendering (`vanilla?`, file key `vanilla-rendering?`) is upstream's path: the effective
settings are the engine's boot state (`sandover-boot-state!`), so nothing is pushed and the frame is
upstream's, MSAA at the stock setting. It is the parity path and the choice for players who do not
want temporal AA. The engine reports `pc-temporal-aa-state`: 0 when it cannot run temporal AA
(below GL 4.3, or the FSR 3.1 programs did not build; one log line, `fsr3: ...vanilla rendering in
its place`), which `sandover-vanilla?` treats as vanilla rendering; 1 when it runs; 2 when it runs
per eye (stereo or XR), where contact shadows do not run (`sandover-stereo-engine?`).

Rows that need temporal accumulation key off `(sandover-temporal-aa? settings)` (`pc/pckernel.gc`):
not vanilla rendering and state 1 or 2. `(sandover-temporal-upscaler?)` (`pc/pckernel-impl.gc`) is the
same test on the current settings, for menu rows (it ignores an argument, kept for older call
sites).

REPL only: `pc-set-aa` 2 the SDK debug view (0/1), 3 sharpness (0..1, default 0.8), 4 jitter (1
on; 0 off, 2 and 3 negate y or x: diagnostics), 5 resets 2-4. `pc-set-aa 0 0` switches the engine
to MSAA with the effects on (a perf comparison, `sandover-perf --set aa`).

## Render scale

`render-scale` (100, 77, 67, 59, 50, 33: percent of the display size; `pc-set-aa 1`) renders the
scene smaller than it is shown, and FSR 3.1 reconstructs it at the display size. Each entry shows
FSR 3.1's quality name for its ratio and the render size at the current display size:

| Entry | Ratio | Label | At 3840x2160 |
|---|---|---|---|
| 100% | 1 | NATIVE | 3840x2160 |
| 77% | 1.3 | (none: no FSR 3.1 preset) | 2954x1662 |
| 67% | 1.5 | QUALITY | 2560x1440 |
| 59% | 1.7 | BALANCED | 2259x1271 |
| 50% | 2 | PERFORMANCE | 1920x1080 |
| 33% | 3 | ULTRA PERFORMANCE | 1280x720 |

The size text is computed when the row draws, from the game resolution the menu would send
(`sandover-display-size`), so it follows resolution changes. Below 100 % the order is: scene and
every scene-space pass (SSAO, tile lights, shadows, bloom, sprites with the 2D HUD and the 3D HUD
icons) at the render size, FSR 3.1 to the display size, then the `debug`, `debug-no-zbuf` and
`subtitle` buckets (HUD and menu text, letterbox, blackout, subtitles) at the display size, then
the final pass (tonemap, HDR output encode) as before. The size is applied where the frame's sizes
are chosen (`pipelines/opengl.cpp`) while temporal AA runs, and in XR to the eye frame (VR profile,
above); vanilla rendering always renders at the display size. Rig screenshots read the display-size image after the
text buckets and before the HDR encode. The sprite HUD is reconstructed from the render size below
100 %; text is always sharp.

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
- Ambient occlusion on, or a quality change while on, sends `pc-set-ssao-preset <tier>` and the
  emissive mask, replacing any REPL-tuned SSAO parameter.
- Actor shadows on sends `pc-set-shadows 1 1.0` and the 0.3 landing disc; a shadow quality change
  sends `pc-set-shadow-option 40 <tier>` (a row whose switch is off counts as high).
- Sun sends `pc-set-sun-option 10` on its own; neighbour levels no longer switch the disc.
- Motion blur sends `pc-set-motion-blur 0` with the level; reapply resets its shutter, radius cap
  and debug view (`pc-set-motion-blur 7`).
- Camera motion blur sends `pc-set-motion-blur 10` with the mode; the update sends the mounted
  flag (`pc-set-motion-blur 11`) when it changes; reapply puts both back to off.
- Bloom on sends enable and the hue clip; other switches send only their enable.

Presets stay in charge of the scalars: the menu exposes switches, quality tiers and the
technical choices (vanilla rendering, FP16, HDR output format, upscaler, render scale) only. A look
change reloads `presets/pbr.toml` (`pc-set-pbr-param 11`), which resets every PBR scalar, so a
menu slider over a preset scalar would be wiped by the next look change.

## Add a setting

1. Field in `sandover-settings` (`pc/pckernel-impl.gc`): `symbol` for on-off, `int32` index for
   a list. Default in `sandover-settings-reset!`; the VR profile gets it too (the same struct),
   with a VR-only default in `sandover-settings-reset-vr!` only when VR needs another. Bump `SANDOVER_SETTINGS_VERSION` only if an
   old value must be migrated (then migrate after `dosettings` in `sandover-settings-read`).
2. Key in `sandover-settings-read-key` and `sandover-settings-write` (`pc/pckernel.gc`); lists
   are written as names, not indices.
3. Boot state in `sandover-boot-state!`, the effective rule (if it depends on another
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
