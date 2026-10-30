# Lightbench how-to (Jak 1)

`lightbench` renders a level's background (tfrag, tie, shrub) straight from its FR3 file,
with a camera and time-of-day weights you choose, and writes a color PNG, a linear depth
buffer and a texture-id buffer. It runs without goalc or gk, so it can be used while the
game is busy, and in a loop. Findings:
`sandover/kb/findings/2026-09-23-lightbench.md`.

Prerequisites: a Release build tree and the extracted assets
(`sandover/kb/howto/build-and-extract.md`); `out/jak1/fr3/<level>.fr3` must exist.
`OPENGOAL_PROJ_PATH` must point at the checkout.

## Build

Only the one target, so the build stays short (hold `$SANDOVER_DATA/locks/build.lock`):

```sh
ninja -C out/build/Release -j <logical threads - 2> lightbench
```

Source: `tools/lightbench/lightbench.cpp`, registered in `tools/CMakeLists.txt`. It links
the `runtime` static library and changes no engine file.

## Run

```sh
out/build/Release/bin/lightbench --continue village1-hut --hour 12
```

Outputs go to `sandover/data/lightbench/` in the checkout (gitignored; `$SANDOVER_DATA` is not read) unless `--out` is given, named
`<prefix>-*` (prefix defaults to the continue point):

| File | Content |
|---|---|
| `-color.png` | RGBA8 render, alpha forced to 255 |
| `-depth.npy` | float32 (H, W), meters along the view axis, `inf` where nothing was drawn |
| `-depth.png` | log-scaled preview of the above (near = white) |
| `-texid.npy` | uint32 (H, W): bits 0-23 texture id, 24-27 renderer (1 tfrag, 2 tie, 3 shrub), 28-31 level index; 0 = nothing |
| `-texid.png` | false-color preview of the above |
| `.json` | camera, weights, fog, per-level texture table (`id` -> name, tpage, combo_id, size), timings |

Texture id `n` of level `L` is `levels[L].textures[n-1]` in the JSON. The id pass keeps each
draw's alpha test, so cut-out foliage has the right silhouette; the last draw to touch a
pixel wins, translucent ones included.

## Options

| Option | Meaning |
|---|---|
| `--continue NAME` | continue point from `goal_src/jak1/engine/level/level-info.gc` (camera-trans, camera-rot, lev0/lev1) |
| `--levels a,b` | FR3 levels to draw (default: the continue point's lev0 and lev1) |
| `--cam-trans x,y,z` / `--cam-rot 9 floats` | override the camera; GOAL units (4096 per meter), `camera-rot` is `inv-camera-rot` rows, as printed by `trsq->continue-point` |
| `--hour H` | time of day from the level's mood tables (see below) |
| `--solo N` | palette slot N at weight 1.0, all others 0 |
| `--itimes w0,...,w7` or 24 values | explicit per-slot weights (1.0 = full), or r,g,b per slot |
| `--slot-weight N=W` | set one slot after the above, e.g. `5=0.5` for village1's flame slot |
| `--fog-color r,g,b`, `--fog-dists start,end,max,min`, `--no-fog` | fog override (defaults from `--hour`, off otherwise) |
| `--width`, `--height`, `--msaa`, `--fov` | output size, color-pass samples, horizontal FOV at 4:3 in degrees (default 64, the game default) |
| `--no-tfrag`, `--no-tie`, `--no-shrub`, `--no-ids` | skip a renderer or the id pass |
| `--repeat N` | timed color-pass repetitions (first frame includes GPU setup) |

`--hour` mirrors `update-mood-palette` and `update-mood-fog`: it reads
`sandover/tools/data/mood/<level>.json`, a JSON copy of that level's `palette-interp`,
`fog-interp` and `mood-fog-table` from `goal_src/jak1/engine/gfx/mood/mood-tables.gc`. The
GOAL table stays the source of truth; regenerate the mirror after editing it:

```sh
cd sandover/tools
uv run python -m sandover.mood_tables village1 beach
```

A level without a mirror falls back to the first level's. Level effects (flames, lightning,
light flicker) are not simulated; add them with `--slot-weight`.

## How it works

- Hidden SDL window and a GL 4.3 core context, as in `game/graphics/gfx_test.cpp`.
- `Loader` loads `GAME.fr3` then the requested levels (`update_blocking`).
- One `TFragment`, `Tie3WithEnvmapJak1` and `Shrub` per level, drawn in bucket order:
  tfrag opaque and tie per level, shrub, then tfrag trans/dirt/ice/water.
- The camera is built the way the GOAL side builds `TfragPcPortData`: perspective from
  `update-math-camera` (NTSC, PC wide-aspect rule), `camera-rot` from `update-camera`. TIE and
  shrub get it through a small synthetic DMA chain; tfrag through its public API. Frustum
  planes accept everything, so culling is left to the depth test.
- Texture-id pass: the background shaders are swapped for copies that keep the upstream
  vertex shader and output an integer id to an `R32UI` target (blending does not apply to
  integer targets). The generated shader files land in `out/lightbench/shaders/`.

## Headless

Needs a desktop session on Windows: `SDL_VIDEODRIVER=offscreen` fails
(`eglQueryDevicesEXT is missing`) and `dummy` has no GL. On Linux the offscreen driver with
EGL device enumeration (vendor EGL, or Mesa llvmpipe for CPU-only) is the route to try.
