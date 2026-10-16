# Sandover

A lighting and materials remaster of *Jak and Daxter: The Precursor Legacy*, built on
[OpenGOAL](https://github.com/open-goal/jak-project). Sandover keeps the game, its levels and its
art direction, and changes how the world is lit and shaded:

- shadows from the sun, the moon and local lights (lamps, crystals, eco) on actors and the world
- ambient occlusion, and light cast by glowing surfaces: eco, lava, lamps
- PBR materials (normal and roughness per texture), fur, ice, metals that take the level's light
- reworked water: clear inland water, wet shorelines, sun glint shadowed by the world
- neighbouring levels drawn for real in the distance, with fog at least as dense as the original
- a sun, moon and stars in the style of *Jak 3*
- HDR10 output with an in-game calibration page, SMAA, render scale with FSR 1

**Status: public beta, 0.9.0.** A partial playthrough by the maintainer found no crashes or
progression blockers; see [Known issues](#known-issues).

## Screenshots

<!-- Before/after pairs (stock OpenGOAL vs Sandover) go here for the release. -->
_Coming with the release announcement._

## Requirements

- **Windows**, 64-bit. Tested on Windows 11 with a current NVIDIA GPU and driver. Other GPUs and
  Windows 10 should work (OpenGL 4.3, a CPU with AVX) but are untested.
- **The [OpenGOAL launcher](https://opengoal.dev)**, a recent version with mod support.
- **Your own copy of the game** as a disc image (`.iso`) made from your own disc. Nothing from
  the game is in this repository; what the download holds is listed under [Install](#install).
  If you already play Jak 1 in the launcher, the mod reuses the disc data you gave it.

### Disc version

Sandover's lighting data is built for the **original North American release (NTSC-U 1.0)**.

- NTSC-U 1.0: everything works as intended.
- Other releases (PAL, NTSC-J, later NTSC-U printings) should work, untested; only NTSC-U 1.0
  was tested. Where a level's geometry differs from NTSC-U 1.0, the differing parts keep the
  original lighting, and the game log names the level (`unbake: <level> ...`).

## Install

The OpenGOAL launcher is the only supported way to install Sandover.

1. In the launcher, open **Settings > Mods**, paste this into **Mod Source URL** and press **Add**:

   ```
   https://raw.githubusercontent.com/bouzou4/project-sandover/master/sandover/release/mod-source.json
   ```

2. Open **Jak and Daxter**, then **Features > Mods**. Pick **Sandover (beta)** under
   **Available Mods** and install it. The launcher downloads the release, builds the game from
   your disc data (a few minutes) and gives the mod its own **Play** button.
3. Optional, for HDR: turn on **Use HDR** for your display in Windows Settings > System > Display
   before starting the game, then set peak brightness, paper white and black level on
   **Options > Sandover > HDR CALIBRATION**.

Updates arrive through the same mod source. Sandover keeps its own settings and saves, separate
from the stock game in the launcher.

The download (about 400 MB) holds the engine, the game's source code, and data Sandover derived
offline from the game's files: per-texture PBR maps, canopy maps and per-level lighting sidecars
for NTSC-U 1.0. It also includes five sun and moon sprite images taken from *Jak 3*. Nothing
else from any disc is included, and your own Jak 1 disc image is still required.

## Settings

Everything is on by default. **Pause > Options > Sandover** has:

- an **on/off row for each enhancement**: AMBIENT OCCLUSION, EMISSIVE LIGHT, BLOOM,
  ACTOR SHADOWS, MATERIALS, FUR, WATER, SUN GLINT ON WATER, NEIGHBOUR LEVELS, SUN,
  FP16 COLOR
- **LOW / MEDIUM / HIGH** only for the two heavy ones, AMBIENT OCCLUSION QUALITY and
  SHADOW QUALITY (HIGH by default)
- **HDR OUTPUT**: PQ (HDR10, the default), SCRGB, or off, and the **HDR CALIBRATION** page. The
  game presents HDR only while Windows HDR is on for its display; otherwise it runs in SDR.
- **SMAA**, on by default, on top of the stock MSAA setting (2x by default)
- **RENDER SCALE** (100%, 77%, 67%, 50%) with an **UPSCALER**, BILINEAR or FSR 1

Some rows depend on others: EMISSIVE LIGHT needs AMBIENT OCCLUSION on, and HDR OUTPUT and
HDR CALIBRATION need FP16 COLOR on. A greyed-out row is waiting for the row it depends on.

On a weaker GPU, render resolution is the safe lever: the game's art was made for PS2
resolution, so rendering below your display's resolution costs far less than it would in a
modern game. Turn individual enhancements off after that.

If exclusive fullscreen looks washed out in HDR, close overlays such as the NVIDIA app overlay
and try again.

## Known issues

- **A stall of up to several seconds when a level first comes into view**, while its PBR
  material maps upload. A fix is in progress.
- **Distant geometry of a neighbouring level overlaps the real level geometry** in some far
  views, seen in the precursor basin and in a lava area. Visual only.
- **Input can stop responding after changing a graphics setting in an in-game menu** while the
  game keeps running. It recovers after a wait.
- **VR mode crashes** shortly after loading. VR is on the 1.0 roadmap; play on a monitor.
- **A thin lit sliver** shows along one contact crease in the jungle (shadow bias).

## Roadmap to 1.0

- VR: fix the crash and pass a full smoke test
- Performance-tiered settings for lower-end GPUs
- Further optimization
- Linux and Steam Deck

## Reporting bugs

Use the in-game bug report, so the problem can be reproduced:

1. Where you see the problem, press **F7** (or **Options > Sandover > SAVE BUG REPORT (F7)**).
   "BUG REPORT SAVED" shows at the top left.
2. The report goes to the `bug-reports` folder in the mod's settings folder, inside the
   launcher's install folder: `features/jak1/mods/Sandover/_settings/sandover/bug-reports`. Each
   report is a PNG of the frame, a JSON sidecar (level, camera, time of day, your Sandover
   settings, display and HDR state, build, Windows version, GPU and driver, the end of the game
   log) and a zip of both. File paths and your user name are removed before it is written.
3. Open an issue with the **Sandover bug report** form, say what you saw and what you expected,
   and attach the zip.

## How it was made

Sandover was built with AI coding agents working under the maintainer's direction. The agents
wrote the code, tools and documentation and reviewed each other's changes; the maintainer set
the direction, judged the results in play and in side-by-side captures, and decided what ships.

## Building from source

Sandover is a fork of OpenGOAL's [jak-project](https://github.com/open-goal/jak-project), and
the upstream build instructions apply. Sandover's own notes are in `sandover/kb/` (start with
`sandover/kb/howto/build-and-extract.md`); release packaging is described in
`sandover/kb/findings/2026-10-08-packaging.md`.

## License and credits

- Built on [OpenGOAL](https://opengoal.dev) by the OpenGOAL team, whose decompilation, compiler
  and PC port make this possible. Sandover is not affiliated with or supported by the OpenGOAL
  team; report Sandover problems here, not to them.
- *Jak and Daxter* is a Naughty Dog game published by Sony Interactive Entertainment. Sandover is
  an unofficial fan project; you need your own copy of the game.
- Source code: ISC License, as upstream (`LICENSE`). Third-party libraries keep their own
  licenses under `third-party/`; OpenAL Soft (LGPL) ships as a separate DLL with its notices,
  and its source is in `third-party/openal-soft` at the release tag.
