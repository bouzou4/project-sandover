---
title: How Sandover ships, to a friend today and as a GitHub release later
date: 2026-10-08
issue: none
status: verified (friend zip installed and booted on the dev machine); release route inferred
sha: b8064d44d
updated: 2026-10-16 (beta 0.9.0: mod source, flat release zip, packaging preset)
---
## Answer (≤5 lines)
A zip with our binaries, the source tree the extractor compiles, our presets and every derived
data set the remaster reads (prebuilt here, sidecars included). 382 MB zipped, 750 MB unpacked.
`sandover-package build` makes it. The only supported install is an OpenGOAL launcher mod source
(`sandover/release/mod-source.json`) listing that zip as a release asset; the launcher runs the
zip's extractor on the user's disc. `install.ps1` in the zip is an unsupported dev tool (it ran
the friend test below). The first public release is the beta 0.9.0.

## What the engine reads at run time (all relative to the project dir)
The project dir is `--proj-path`, else `$OPENGOAL_PROJ_PATH`, else the `data/` folder next to
`gk.exe` (`common/util/FileUtil.cpp` `setup_project_path`). Every Sandover file lookup falls back
from `$SANDOVER_DATA` to `<project>/sandover/data`, so an install needs no environment variables.

| Data | Path | Read by | Ours or derived | Size |
|---|---|---|---|---|
| Shaders | `game/graphics/opengl_renderer/shaders/` | `pipelines/opengl.cpp` | ours (source) | 0.9 MB |
| Assets (fonts, text, controller db) | `game/assets/` | gk, compiler | upstream source | 16 MB (jak1 + fonts) |
| GOAL source, decompiler config | `goal_src/`, `decompiler/config/jak1/` | extractor compile and decompile | ours/upstream source | 13 MB |
| PBR preset | `sandover/presets/pbr.toml` | `Materials.cpp` `pbr_load_preset` | ours | <0.1 MB |
| Material table | `sandover/data/materials/materials.csv` | `Materials.cpp` | derived (texture names and classes) | 0.5 MB |
| Emissive actors | `sandover/data/materials/emissive-actors.csv` | `EmissiveTable.cpp` | derived | <0.1 MB |
| PBR maps (normal, roughness per texture) | `sandover/data/materials/pbr-maps/<tpage>/` | `Materials.cpp` | derived from the disc's textures | 275 MB, 8143 PNG |
| Canopy maps | `sandover/data/lighting/*.canopy`, `lighting/canopy/*.canopy` | `Canopy.cpp`, `CanopyVol.cpp` | derived from level geometry | 186 MB |
| Lighting sidecars | `out/jak1/fr3/<level>.unbake`, else `sandover/data/lighting/<level>.unbake` | `Loader.cpp`, `Unbake.cpp` | derived from level geometry | 190 MB, 21 levels |
| Level geometry, CGOs | `out/jak1/fr3/*.fr3`, `out/jak1/iso/` | gk | the disc, built on install | 1.8 GB |

Not shipped as files, because they are not read from disk:
- Heightfields: built at level load from the fr3 collision mesh (`LevelHeightfield.h`).
- Light census, entity types, level sky table: compiled into `gk` (`WorldLights.inc`,
  `LevelSky.inc`); the census tables and `entity_types.json` are tool inputs only.
- The other presets (`ssao`, `fur`, `water`, `emissive`, `vista`, ...): their chosen values are
  the engine defaults in `gfx.h` and the settings defaults in GOAL.
- Settings: `sandover.gc` is written with defaults on first boot; the defaults are the play look
  with everything on (`howto/settings.md`, "The file").

`.unbake` sidecars are checked per tree against a digest of the fr3 vertex positions
(`Unbake.cpp` `unbake_pos_digest`); a mismatch logs and keeps the stock lighting for that tree.
So they are valid only for the same disc release (NTSC-U 1.0, `ntsc_v1`) and the same
extractor; `install.ps1` warns when the disc is another release.

## What ships and what is built on the user's machine
Nothing from the disc image ships, ever: `iso_data/`, `decompiler_out/` and the fr3 and CGO
files stay on the machine that extracted them. The one exception is the five sun and moon sprite
images taken from *Jak 3* (`data/sandover/data/sky/jak3`), which ship by decision and are named in
both READMEs. The derived data sets (lighting sidecars, PBR
maps, canopy maps, material table) ship in the zip on purpose, prebuilt here, so the install is
only the extractor run. Derived data comes from the game's assets, so a public release of it is
the maintainer's call.

| Group | Files | Unpacked | In the friend zip | GitHub release |
|---|---|---|---|---|
| Binaries: gk, goalc, extractor, 21 engine DLLs, app-local VC++ runtime | 29 | 69 MB | yes | yes |
| Source: goal_src (common, jak1, user), decompiler config jak1, assets, shaders, custom_assets jak1, presets | 873 | 29 MB | yes | yes |
| Material table + emissive actors | 2 | 0.5 MB | yes | yes |
| PBR maps | 8143 | 275 MB | yes | yes (size is fine) |
| Canopy maps | 18 | 186 MB | yes | yes |
| Lighting sidecars | 21 | 190 MB | yes | yes |
| Disc extract, decompile, compiled game | | 3.5 GB | no: built by the extractor | no |
| Total shipped | 9089 | 750 MB | 382 MB zip | one 382 MB asset |

The GitHub release decision is by size. Release assets take up to 2 GB per file without LFS;
the whole package is 382 MB zipped, so it fits in one asset and nothing needs deriving on
install. Deriving on install would mean shipping the Python tools and their dependencies (numpy,
scipy, scikit-image) plus a decompile with textures, far more fragile than a 382 MB download;
it is worth building only if the data outgrows one asset by several times.

## install.ps1 flow (unsupported dev tool; friend zip, verified)
Kept for testing a zip without the launcher; not a user route.
1. Extract the zip anywhere with about 5 GB free.
2. Run `Install.cmd` (it runs `install.ps1` with `-ExecutionPolicy Bypass`; a group-policy
   execution policy overrides that). On the first run it unblocks the downloaded files; it asks
   for the `.iso` with a file dialog and checks free space.
3. `extractor.exe <iso> --proj-path <install>\data --validate --extract --decompile --compile`:
   extracts the disc to `data/iso_data/jak1`, rejects a disc that is not in the extractor's
   database (codes 4000-4029, mapped to messages from the shipped `error-code-metadata.json`,
   so a disc that is not a known Jak 1 release fails right after extraction: another game,
   an unknown serial, a modified executable, a wrong file count or contents that match no known
   release; every Jak 1 release in the database passes, whatever its region), decompiles
   to `data/decompiler_out/jak1` and writes the fr3 level files, then compiles `goal_src` with
   `(mi)` into `data/out/jak1`. The log is `extractor.log`.
4. Checks `out/jak1/iso/GAME.CGO`, warns if the disc is not `ntsc_v1` and counts the 21 `.unbake`
   sidecars in `data/sandover/data/lighting/`, where gk reads them, and deletes any old
   `.unbake` copies in `data/out/jak1/fr3/`, which would win over them (since #401; before, they
   shipped in `data/sandover/fr3/` and were copied next to the fr3 files).
5. Writes `Play Sandover.cmd` (relative: `%~dp0` and `%APPDATA%\Sandover`, so non-ASCII user
   names and folders work) and a desktop shortcut:
   `gk.exe -v --game jak1 --proj-path <install>\data --config-path %APPDATA%\Sandover -- -boot -fakeiso`
   (the launcher's own boot arguments, `open-goal/launcher` `mods.rs` `generate_launch_mod_args`).

`--proj-path` is explicit so a stray `OPENGOAL_PROJ_PATH` (set on dev machines) cannot redirect
the game. The `.cmd` also clears `SANDOVER_DATA`; the shortcut cannot, which matters only on a
machine that sets it.

## Verification (dev machine, NTSC-U 1.0 disc)
- Fresh folder, `install.ps1 -IsoPath ... -ShortcutDir <test> -ConfigDir <test>` under Windows
  PowerShell 5.1: exit 0 in 185-237 s; 26 fr3 levels, 21 sidecars copied; installed size
  4.3 GB (iso_data 1.4 GB, decompiler_out 0.2 GB, out 1.9 GB, package 0.75 GB).
- gk from the installed folder with `SANDOVER_DATA`, `SANDOVER_ROOT` and `OPENGOAL_PROJ_PATH`
  cleared: reaches `GAMEPLAY: enter title`, presents HDR10, allocates SSAO, writes `sandover.gc`
  with everything on, loads `materials.csv`, `pbr.toml` and the PBR map index from the package.
- Debug boot of the same install, warped to `village1-hut` over the REPL: every sidecar loaded
  matches the freshly extracted geometry (`unbake: village1 7/7 trees match`, beach 3/3,
  jungle 4/4, misty 5/5, firecanyon 3/3, training 4/4, village2 5/5, rolling 3/3); canopy maps
  and heightfields load. The fresh fr3 files differ in bytes from the dev checkout's
  (extracted with a levels-only override) but not in vertex positions.
- 2026-10-09, zip from 3c5242d4a: a 1 MB blank `.iso` is rejected with code 4041 and the
  "could not be read" message; the real disc installs (exit 0, 372 s) with the default config
  dir; `Play Sandover.cmd` (relative paths) boots to the title in HDR10 and writes its settings
  under `%APPDATA%\Sandover`.
- First attempt aborted at boot (`0xC0000409`): `REPL::find_repl_username` lists
  `goal_src/user`, which the package had left out; it now ships (upstream ships it too).

## Saves and settings
`--config-path %APPDATA%\Sandover` puts settings (`pc-settings.gc`, `sandover.gc`), saves and
screenshots under `%APPDATA%\Sandover\OpenGOAL\jak1\`, apart from a stock OpenGOAL install
(`%APPDATA%\OpenGOAL\jak1`). The save format is stock, so saves can be copied across by hand.
Reinstalling or updating never touches that folder.

## First run
gk boots straight to the title (no REPL, no debug mode). On the first boot the game writes
`sandover.gc` with defaults (everything on, quality high, HDR output PQ requested, 2x MSAA plus
SMAA since #390) and the stock `pc-settings.gc`. Shader programs compile on first use (no shader cache exists).

## Windows HDR
HDR output is requested by default but the engine presents HDR only when Windows HDR ("Use HDR")
is on for the display the window is on; otherwise it stays SDR and logs why
(`HdrPresent.cpp`). It needs Windows 10/11 and a driver exposing `WGL_NV_DX_interop2` (current
NVIDIA, AMD and Intel drivers do). Option 7 (switch Windows HDR on automatically) stays off.
The calibration page (Options > Sandover > HDR calibration) is the user-facing step. HDR10 (PQ)
is the default and scRGB an option. Exclusive fullscreen looked washed out in HDR in play on
2026-10-15/16 and did not reproduce after overlays were closed; the mechanism is a hypothesis
(`symptoms.md`). The README advises closing overlays such as the NVIDIA app overlay if it
happens.

## Updates
Through the launcher: a new version in the mod source's version list; the launcher reuses the
user's `iso_data` and reruns decompile and compile. The `install.ps1` dev route has no update:
extract the new zip to a new folder and run it again (settings and saves live in
`%APPDATA%\Sandover` and carry over).

## Supported install: an OpenGOAL launcher mod source
The OpenGOAL launcher already installs mods: a user adds a mod-source URL (a JSON file:
`schemaVersion`, `sourceName`, `mods.<name>.versions[].assets.windows` = a release asset URL;
`open-goal/launcher` `src/lib/rpc/bindings/ModSourceData.ts`, `ModVersion.ts`). On install it
downloads the zip into `features/jak1/mods/<source>/<mod>/`, runs that zip's own `extractor` on
the disc image (or the `iso_data` the user already has from the base game), decompiles and
compiles with the zip's `data/`, and launches its `gk` with a per-mod settings folder
(`src-tauri/src/commands/features/mods.rs`). Our zip already has that layout (exes plus `data/`,
the upstream release layout from `.github/scripts/releases/extract_build_windows.sh`).

Why this over the others:
- The audience (OpenGOAL players on Reddit) already has the launcher and a validated disc
  image in it; bring-your-own-ISO, validation, updates (version list), per-mod saves and
  uninstall come for free, on Windows, Linux and macOS.
- Inno Setup would give a polished single `.exe` with an uninstaller, but we would own the
  extractor orchestration, error reporting, updates and an unsigned-installer SmartScreen
  warning; it duplicates what the launcher does.
- The PowerShell zip works and needs nothing installed, but execution-policy and
  Mark-of-the-Web friction, no uninstaller, no update path. It stays an unsupported dev tool.

Launcher facts the package depends on (`open-goal/launcher` main, read 2026-10-16):
- The mod-source JSON follows `schemas/mod-source/v1/mod-source-schema.v1.json` (required:
  `schemaVersion`, `sourceName`, `lastUpdated`, `mods`, `texturePacks`; per mod `displayName`,
  `description`, `authors`, `tags`, `supportedGames`, `versions`; per version `version`,
  `publishedDate`, `assets`). Ours validates against it and matches the Rust structs in
  `src-tauri/src/cache.rs`.
- The asset is extracted into `features/jak1/mods/<sourceName>/<mod>/` with no top folder
  stripped (`extract_and_delete_archive(..., false)` in `mods.rs`), and `extractor`/`gk` are
  run from that folder. So a release zip must be flat: `sandover-package build --version X.Y.Z`
  writes `sandover-vX.Y.Z-windows.zip` with no `sandover-<sha>/` folder (the dev zip keeps it).
- gk gets `--config-path features/jak1/mods/<sourceName>/_settings/<mod>` and no `--proj-path`, so
  it finds `data/` next to itself; settings, saves and bug reports (`<config>/bug-reports`) live
  in that `_settings` folder.

Release steps (Windows, the maintainer's machine):
1. `cmake --preset Release-windows-clang-package`, then
   `cmake --build out/build/Package --target gk goalc extractor` (holding the build lock).
2. `sandover-package build --build out/build/Package/bin --version 0.9.0`. With `--build` the
   binaries, the extractor and OpenAL Soft's notices come from that build tree and `build.json` is
   computed from the checkout; the skew and dirty-source checks are unchanged. The VC++ runtime is
   added only when a binary imports it, so the static build ships none.
3. `gh release create v0.9.0 sandover-v0.9.0-windows.zip` on the public repo; bump
   `publishedDate`/`lastUpdated` in the mod source to the release time and push it.

What the launcher route needs from us:
- Done (#401): the package ships the `.unbake` sidecars in `data/sandover/data/lighting/` and gk
  reads them there, so no installer step is needed. `unbake_path` takes the first that exists of
  `out/jak1/fr3/<level>.unbake`, `$SANDOVER_DATA/lighting/<level>.unbake` and
  `<project>/sandover/data/lighting/<level>.unbake`, and logs `unbake: <level> sidecar <path>` or
  `unbake: <level> no sidecar found, stock lighting`; the per-tree digest check is unchanged.
  On a machine that sets `SANDOVER_DATA`, a launcher install reads that folder's sidecars before
  its own (canopy and material data already behave this way).
- A public repo or a public assets host (the launcher downloads anonymously), and a mod-source
  JSON hosted at a stable URL (a file in the repo, served raw, is enough).
- Release builds are made locally (`sandover-package build`) and uploaded with
  `gh release create`; GitHub Actions stay off on the private repo.
- Name: `Sandover` is in `sourceName`, `displayName` (`Sandover (beta)`) and the zip name. The mod
  source URL in the README assumes the public repo keeps the repository owner and name and
  that its default branch is `master`.

## Known issues and blockers
- The dev build is a shared-library build (21 DLLs) linked against the VC++ 2015-2022 runtime;
  a zip made from it carries `msvcp140*.dll` and `vcruntime140*.dll` app-locally from Visual
  Studio's redistributable folder. The release uses the `Release-windows-clang-package` preset
  instead (own build tree `out/build/Package`): `STATICALLY_LINK` (static libraries, as upstream's
  `Release-windows-clang-static`) plus the static C++ runtime (`CMAKE_MSVC_RUNTIME_LIBRARY`
  `MultiThreaded`, with policy CMP0091 forced NEW because the top-level minimum version predates
  it). 20 of the 21 DLLs and the five runtime DLLs go away; `OpenAL32.dll` stays shared on purpose
  (OpenAL Soft is LGPL; `CMakeLists.txt` builds it `SHARED` in every configuration) and ships
  with its `OpenAL-Soft-*.txt` notices.
- The package uses the newest auto-staged build and refuses one whose `build.json` engine commit
  differs from the checkout's (shaders and GOAL come from the checkout); `--allow-skew` overrides.
  It also refuses when the shipped source (goal_src, shaders, presets, assets) has uncommitted
  edits, which would otherwise ship under a clean commit.
- `extractor.exe` comes from the build tree (staging copies only gk/goalc). Its code lives in the
  shared libraries, so the package requires those to be byte-identical to the staged copies and
  the exe to be newer than `decompiler/extractor`; the skew check covers both.
- Disc releases other than NTSC-U 1.0 extract and run, but keep stock lighting where the
  sidecar digests do not match (logged per level). The README states this as the disc-version
  compatibility; the other enhancements are untested on those releases.
- gk and the installer are unsigned: SmartScreen asks once.

## Follow-ups
- The packaging preset is full-build verified: gk, goalc and extractor built, and the binaries
  import only system DLLs and `OpenAL32.dll`. Pending: a gk boot of the static build and a
  launcher install of the resulting zip; then the release steps above.
- Code signing if the release grows.
