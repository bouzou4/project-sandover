---
title: Can a standalone tool render a level's background from FR3 with a synthesized camera and time of day?
date: 2026-09-23
status: verified
sha: f3e5d401f
---
## Answer (≤5 lines)
Yes. `lightbench` (`tools/lightbench/`) links `runtime`, loads `GAME.fr3` plus the continue
point's lev0/lev1 through `Loader`, and draws tfrag, tie (with envmap) and shrub, fully
textured, from `village1-hut` at hour 12, in about 2.5 s per process (steady frame about 5 ms
at 1920x1080). It writes color, linear depth and a texture-id buffer. Sky, ocean, actors,
sprites and level effects are not drawn, and the continue-point camera is not the settled
camera the rig captures, so FLIP against the rig is high (0.64 over covered pixels).

## Evidence (path:line@sha / URL, short excerpts)
Runtime checks (Windows, desktop GPU, GL 4.3 core, hidden SDL window):

| Check | Result |
|---|---|
| Hidden window + GL 4.3 context, as `game/graphics/gfx_test.cpp:26-33` | works; the `SharedRenderState` shader library compiles unchanged |
| `Loader` driven directly (`set_want_levels` + `update_blocking`, `loader/Loader.cpp:59,358`) | village1 + beach loaded, all level textures uploaded |
| `TFragment::setup_for_level` + `render_matching_trees` (`background/TFragment.h:52-72`) | draws; no DMA needed |
| `Tie3WithEnvmapJak1::render` and `Shrub::render` fed a synthetic CNT/END DMA chain in the layout of `Tie3::set_up_common_data_from_dma` (`background/Tie3.cpp:276-367`) and `Shrub::render` (`background/Shrub.cpp:17-68`) | draws, including envmap second pass and wind trees (zero wind) |
| Renderer-private state | not needed: the DMA chain carries `TfragPcPortData`, `WindWork` and the envmap color |
| Textures (open question 12 in `facts/renderer.md`) | complete for village1/beach; Jak 1 tfrag/tie use no anim slots, so `anim_slot_array = nullptr` is fine |
| `make_new_cam_mat` (`background/background_common.cpp:169`) | not in the header; declared locally and reused to invert depth |
| Texture ids | shaders swapped for copies with the upstream vertex shader and an integer fragment output into `R32UI`; blending is ignored for integer targets, alpha test kept |

Camera math, mirrored from GOAL: perspective from `update-math-camera`
(`goal_src/jak1/engine/gfx/math-camera.gc:54-204`: d 1024, f 40960000, x-pix 256, y-pix 112,
PC rule y-ratio = 0.75 tan(fov/2), x-ratio widened by aspect/(4/3)); `camera-rot` =
transpose of `inv-camera-rot` with row 3 = -trans x rot, `camera-temp` = rot x persp
(`engine/camera/cam-update.gc:316-352`); continue-point `camera-rot` is `inv-camera-rot`
(`engine/game/game-info.gc:615-637`). Default fov 11650.845 = 64 degrees
(`math-camera.gc:240`).

itimes: `times[p].xyz` is 1.0 (`engine/gfx/mood/mood-h.gc:72-79`), so each slot's weight w
packs as `int(w*4096)>>6` per channel, two slots per quadword (`engine/gfx/mood/mood.gc:30-133`),
matching the C++ unpack in `interp_time_of_day`. `--hour` mirrors `update-mood-palette`
(`mood.gc:146-204`) and `update-mood-fog` (`mood.gc:240-257`) from a JSON copy of
`mood-tables.gc` (`sandover/tools/data/mood/*.json`, `sandover.mood_tables`).

Timings (1920x1080, village1 + beach, warm disk):

| Stage | ms |
|---|---|
| SDL + GL context | 350-1270 |
| shader library | 30-450 |
| `GAME.fr3` | 80-470 |
| village1 + beach FR3 | 480-610 |
| first frame / steady frame | 13-50 / 4.7-5.6 |
| readback (color + depth) | 15-24 |
| texture-id pass (incl. shader compile, id textures) | 270-440 |
| whole process | about 2.5 s (1.3 s at 480x270 without ids) |

FLIP (`flip-evaluator`, LDR) against `sandover/data/shots/stage0/village1-hut-noon.png`,
same resolution, lightbench with `--msaa 8`: full frame 0.758; over pixels lightbench
covers (84%) 0.644; at 1/8 scale 0.61. Mean RGB over covered pixels: rig
(0.62, 0.72, 0.35), lightbench (0.62, 0.70, 0.22); red and green agree, blue differs where
the rig shows sky and sea behind geometry lightbench has. The error is dominated by
misregistration: the rig captures the follow camera 5 s after spawn, lightbench the spawn
camera. A Nelder-Mead fit of camera offset to the capture converged to a degenerate pose
(it maximized coverage), so it was dropped.

Headless: `SDL_VIDEODRIVER=offscreen` fails (`eglQueryDevicesEXT is missing`), `dummy` has
no GL. The hidden window needs a desktop session on Windows.

## Implications for the design
- Unbake can render per-slot images (`--solo N`, `--itimes`) of the real engine shaders offline,
  with depth and texture ids for masking, at about 5 ms per frame once loaded.
- Pixel parity with the rig needs the rig to record the actual camera (`*math-camera*` trans
  and `inv-camera-rot`, as `trsq->continue-point` prints) and feed it via
  `--cam-trans/--cam-rot`; then FLIP over a background mask becomes meaningful.
- Per-process cost (about 2.5 s) is load and context, not rendering: a batch mode (many
  cameras/weights per process) is the obvious next step for sweeps.

## Follow-ups
- Rig: log the camera at screenshot time; re-run FLIP with that camera.
- Batch mode (JSON job list) to amortize context and FR3 load.
- Missing layers: sky, ocean (village1's sea), merc/generic actors, sprites, flames and
  other mood effects (add with `--slot-weight`), mood-derived envmap tint (fixed 2.0), wind.
- Headless on Linux via SDL offscreen + EGL (vendor EGL or Mesa llvmpipe).
- Upstream `update_render_state_from_pc_settings` and the renderers assume a single
  `SharedRenderState` per frame; lightbench resets `has_pc_data` per frame.
