# Architecture map: what the fork adds over OpenGOAL

For a reader who knows OpenGOAL (the Jak 1 PC port: GOAL runtime, `gk`, `goalc`, the bucket
renderers in `game/graphics/opengl_renderer`) but not Sandover. Line numbers
are at master `d10288cc2` and drift, function names do not. Short paths are relative to their root: C++ under `game/graphics/opengl_renderer/`, shaders under its `shaders/`, GOAL under `goal_src/jak1/`, presets under `sandover/presets/`, how-tos under `sandover/kb/howto/`; other paths are repository-relative. Every enhancement is
Jak 1 only and sits behind its own setting; with everything off (the Vanilla rendering row) the
engine takes upstream's path.

Scale against the upstream merge base `76e8eeca3`: about 1,700 files, mostly data (shot
manifests, presets, kb, tools, the vendored `third-party/openxr`, `openal-soft`, `fidelityfx`,
`amd-fsr4`, `nvidia-dlss`). Engine code: 37 new renderer units (`.cpp`) under
`game/graphics/opengl_renderer/`, 87 new and 29 changed shaders, 30 GOAL files.

## Subsystems

Bridge functions are the GOAL-callable `pc-*` symbols registered in
`init_common_pc_port_functions` (`game/kernel/common/kmachine.cpp` calls `register_sandover_bridge`; the
72 entries sit in one table in `game/kernel/common/sandover/bridge.cpp`, the functions in
`sandover/bridge_<feature>.cpp` by subsystem) and declared in `goal_src/jak1/kernel-defs.gc:574-810`. Values travel as integers in millionths;
`which` selects a field of `Gfx::g_global_settings` (`game/graphics/gfx.h`).

### Lighting and shadows

| Part | Where |
|---|---|
| C++ | `background/Unbake.cpp` (per-vertex sidecar relight, `tfrag3_sun`), `SunDisc.cpp` (sun, moon, sky bodies), `WorldLights.cpp` + `LightTracker.cpp` (world light table, slot tracking), `EmissiveLights.cpp` (tile lights), `EmissiveTable.cpp`, `ActorShadows.cpp` (actor maps, landing disc), `WorldShadows.cpp` (world sun map), `ContactShadows.cpp`, `LevelHeightfield.cpp`, `background/Canopy.cpp`, `background/CanopyVol.cpp`, `background/PaletteGlow.cpp`, `SsaoRenderer.cpp` + `SsaoDepth.cpp` + `StencilMarks.cpp` + `TemporalAccum.cpp` (GTAO), `BloomRenderer.cpp` |
| GOAL | `engine/game/main.gc` (`pc-world-lights-update`, `pc-eco-lights-update`, `pc-launch-lights-end`, `pc-lava-light-update`, per frame at `:779-785` after `(update *pc-settings*)`), `engine/gfx/sprite/sparticle/sparticle-launcher.gc` (launch lights, light shafts hidden), `engine/draw/drawable.gc` (`pc-set-shadows`), `engine/gfx/mood/time-of-day.gc` (sky bodies) |
| Shaders | `tfrag3_sun.vert`, `sun_*`, `sky_*`, `world_shadow*`, `actor_shadow_*`, `merc2_shadow.*`, `generic_shadow.*`, `emissive_*`, `ssao_*`, `temporal_accum.frag`, `contact_shadow*`, `bloom*`, `canopy_frag.glsl`, `level_height.glsl`, `cave_dark_frag.glsl`, `atmo_uniforms_vert.glsl`, `atmo_fog_vert.glsl`, `atmo_fog_merc_vert.glsl`, `atmo_combine_vert.glsl`, `em_value_vert.glsl`, `merc_clip_axis_vert.glsl`, `emissive_mask_frag.glsl`, `normal_decode_frag.glsl`, `blur_vec.glsl` |
| Data | `<level>.unbake` sidecars (`Unbake.cpp:323`: fr3 dir, then `$SANDOVER_DATA/lighting`, then `<project>/sandover/data/lighting`), `*.canopy`, `sky/jak3/*.png`; compiled in: `WorldLights.inc`, `SelfLitTextures.inc`, `EcoLights.inc`, `LaunchLights.inc`, `SsaoPresets.inc` |
| Bridge | `pc-set-sun-option`, `pc-set-sun-dir`, `pc-get-sky-bodies`, `pc-set-shadow-option`, `pc-set-emissive-lights`, `pc-world-lights-*`, `pc-eco-lights-*`, `pc-launch-lights-*`, `pc-set-ssao-*`, `pc-set-bloom` |
| How-to | `howto/lighting.md`, `lights.md`, `shadows.md`, `ssao.md`, `level-heightfield.md`, `lightbench.md` |

### Materials

| Part | Where |
|---|---|
| C++ | `Materials.cpp` (class table, PBR maps, looks, parallax), `foreground/Merc2.cpp` (fur shells, actor PBR), the PBR hooks in `background/Tie3.cpp`, `TFragment.cpp`, `Shrub.cpp`, `foreground/Generic2_OpenGL.cpp` |
| Shaders | `pbr_bg_vert.glsl`, `pbr_bg_frag.glsl`, `pbr_common_frag.glsl`, `pbr_albedo_grad_frag.glsl`, `fog_tint_frag.glsl`, `iw_column_frag.glsl`, changed `tfrag3.*`, `etie*`, `shrub.*`, `tie_wind.*`, `merc2.*`, `generic.*` |
| Data | `$SANDOVER_DATA/materials/materials.csv`, `emissive-actors.csv`, `pbr-maps/index.json` + maps; `sandover/presets/pbr.toml` (read at renderer start, `Materials.cpp:1692`); compiled in: `SandTextures.inc` |
| Bridge | `pc-set-pbr-param`, `pc-set-fur`, `pc-set-inspect`, `pc-inspect-pick` |
| How-to | `howto/materials.md`, `inspect.md`, `viewer.md` |

Shared shader snippets are rows of `kSnippets` in `Shader.cpp` plus a `.glsl` file, never copies; `LEVEL_HEIGHT_GLSL` stays last; a token used twice in one stage needs `every_use` or an include guard.

### Water, ocean, lava, swamp

| Part | Where |
|---|---|
| C++ | `ocean/CommonOceanRenderer.cpp` (tessellated near and mid ocean, shading), `InlandWater.cpp`, `LavaField.cpp`, `background/Caustics.cpp`, `Swamp.cpp` (swamp and sea mist) |
| Shaders | `ocean_water.{vert,tesc,tese}`, `ocean_common.*`, `water_waves.glsl`, `water_sun_glint.glsl`, `swamp_mist.*` |
| Data | `presets/water.toml`, `inland-water.toml`, `shore.toml`, `wetpbr.toml`, `swamp.toml`, `misty.toml`, `lava.toml`, `caustics.toml`, `water-glint.toml`; compiled in: `InlandWater.inc`, `LavaVolumes.inc`, `background/CausticMoods.inc` |
| Bridge | `pc-set-water`, `pc-set-inland-water`, `pc-set-swamp`, `pc-set-lava`, `pc-lava-light`, `pc-set-caustics` |
| How-to | `howto/lighting.md` sections 7 (ocean) and "Sun glint" |

### Vista (neighbour levels, unified fog)

| Part | Where |
|---|---|
| C++ | `VistaRenderer.cpp`, `VistaHiZ.cpp`, `Neighbours.cpp`; `pipelines/opengl.cpp` `gl_set_levels` keeps neighbours resident |
| GOAL | `pc/features/vista-landmarks.gc`, `engine/level/level.gc`, `sandover-vista-apply` (`pc/pckernel.gc:1032`) |
| Shaders | the unified fog in the background and merc vertex shaders (`atmo_fog`, `atmo_combine`), `vista_hiz.frag` |
| Data | `presets/vista.toml`; compiled in: `NeighboursMood.inc`, `LevelSky.inc` |
| Bridge | `pc-set-vista`, `-hide`, `-option`, `-fog`, `-neighbour-fog`, `-haze`, `-neighbour-haze`, `-viewer`, `pc-vista-landmarks?`, `pc-vista-neighbour?` |
| How-to | `howto/vista.md`, `facts/vista.md`, ADR-0014 |

### Temporal stack (motion vectors, upscalers, motion blur)

| Part | Where |
|---|---|
| C++ | `MotionVectors.cpp`, `MotionBlur.cpp`, `UpscalerStage.cpp` (which upscaler runs, its per-eye contexts, jitter, reactive mask and hand-off; one `UpscaleInputs` fill), `Fsr3Upscaler.cpp` (FidelityFX FSR 3.1, GL compute), `Fsr4Upscaler.cpp` + `D3d12Bridge.cpp` (FSR 4 through D3D12 interop), `DlssUpscaler.cpp` (D3D11 interop, bring-your-own DLL), `ScreenHistory.cpp`, the jitter injection in `Shader.cpp` (`scene_jitter`) |
| GOAL | camera cuts from `engine/camera/cam-master.gc`, `cam-combiner.gc`; previous-frame bones in `engine/gfx/foreground/bones.gc`; the frame's game step and mounted flag from `engine/draw/drawable.gc` and `sandover-settings-update` |
| Shaders | `motion_vectors*.frag`, `motion_blur*.frag`, `shaders/fsr3/*.comp` |
| Bridge | `pc-set-aa`, `pc-get-aa-available`, `pc-temporal-aa-state`, `pc-set-motion-vectors`, `pc-motion-vectors-dump`, `pc-set-motion-blur` |
| How-to | `howto/settings.md` (Upscaler, Render scale, Motion blur rows), `perf.md`; findings `2026-10-18-fsr3.md`, `2026-10-22-fsr4.md` |

### HDR and present

| Part | Where |
|---|---|
| C++ | `game/graphics/hdr/HdrPresent.cpp` (DXGI swapchain on a child window, HDR10/scRGB, display facts, direct flip), `OpenGLRenderer.cpp` `do_pcrtc_effects`, `set_tonemap_uniforms`, `save_hdr_scene`, `tonemap_to_sdr`; `common/util/exr.cpp`; `screenshot.cpp`, `bug_report.cpp` |
| Shaders | `post_processing.frag` (tonemap, HDR encode, calibration patterns), `tonemap_sdr.glsl` |
| Bridge | `pc-set-hdr-target` (FP16 scene target), `pc-set-tonemap`, `pc-set-hdr-output`, `pc-get-hdr-output`, `pc-bug-report-*`, `pc-snap-poll` |
| How-to | `howto/hdr.md`, `hdr-output.md`, `bug-report.md` |

### XR (OpenXR) and stereo

| Part | Where |
|---|---|
| C++ | `game/graphics/xr/OpenXr.cpp` (instance, session, swapchains, emulated session); `pipelines/opengl.cpp` (60 Hz game tick, held head-turn margin `held_xr_margin_deg`, `publish_xr_cull_view`); `OpenGLRendererXr.cpp` (`OpenGLRenderer` members: `dispatch_buckets_stereo`, `ui_layer_begin/finish`, `copy_eye_to_xr*`, `save_xr_eyes`, `xr_seq_capture`; their state stays in `OpenGLRenderer.h`); per-eye camera in `BucketRenderer.cpp`, `background/background_common.cpp`, `mips2c/jak1_functions/sky_tng.cpp`, `DirectRenderer.cpp` |
| GOAL | `engine/gfx/math-camera.gc`, `engine/camera/cam-update.gc` (the game renders from the head), `engine/gfx/hw/display.gc` (game rate) |
| Shaders | `xr_gs.glsl` (GS-space eye shift), `xr_ui.*` (2D layer) |
| Bridge | `pc-set-xr`, `-xr-recentre`, `-xr-fake`, `-xr-fake-option`, `pc-get-xr-state`, `pc-get-xr-cull`, `pc-set-xr-game-tan`, `pc-get-xr-game-rate`, `pc-set-stereo`, `-stereo-option` |
| How-to | `howto/xr.md`, `vr.md`, `vr-synthetic.md`, ADR-0004 |

### Audio

| Part | Where |
|---|---|
| C++ | `game/sound/989snd/spatial.cpp` (OpenAL Soft bus, output layouts, start-time read of the saved Output row at `:104-118`), `spatial_probe.cpp`, `player.cpp`, `sandover-alsoft-objects.ini`; `game/main.cpp` `--audio-spatial`; the `AudioControl` thread in `game/kernel/common/sandover/bridge_audio.cpp` |
| GOAL | `sandover-audio-*` in `pc/pckernel.gc:891-920`, the Audio page in `pc/progress-pc.gc:948-1028` |
| Bridge | `pc-set-audio-spatial`, `pc-set-audio-output`, `pc-get-audio-state`, `pc-get-audio-string`, `pc-audio-speaker-test` |
| How-to | `howto/audio.md`, ADR-0017 |

### Settings and menus

| Part | Where |
|---|---|
| GOAL | `pc/pckernel-impl.gc:72-155` (`sandover-settings` type, defaults), `pc/pckernel.gc` (file read/write `:21-243`, profiles `:245-318`, apply `:840-1131`), `pc/progress-pc.gc` (pages `:713-1232`, list rows hooked into the stock input and draw code), `engine/ui/progress/progress-h.gc` (screens, `sandover-list` option type), `engine/ui/text-h.gc` (text ids) |
| Strings | `game/assets/jak1/text/game_custom_text_en-US.json`, mirrored in `decompiler/config/jak1/all-types.gc` |
| How-to | `howto/settings.md`, `kb/settings-system.md` |

### Tools, rig and kb

| Part | Where |
|---|---|
| Python | `sandover/tools/sandover`: `rig/` (capture, `rig.gc`, perf, pixdiff), `parity/`, `unbake/`, `lights/`, `materials/`, `vista/`, `triage/` (the interactive review tools are private, not exported), `package.py`, `settings.py` (file mirror), `shader_expand.py` (expands shader includes the way `Shader.cpp` does; the text-identity gate for shader cleanups); CLI names in `sandover/tools/pyproject.toml` |
| Engine hooks | capture mode in GOAL (`rig.gc`), `gk --port` as an option (`game/main.cpp`), `OPENGOAL_MAKE_NO_WRITE` (`goalc/make/Tools.cpp:40`), `PerfTimers.cpp`, `GlQueries.cpp`, `InspectRenderer.cpp` |
| Inputs | shot manifests `sandover/shots/*.yaml`, presets `sandover/presets/*.toml` |
| Docs | `sandover/kb`: `howto/` (procedures), `facts/` (verified engine facts), `findings/` (dated investigations), `decisions/ADR-*.md`, `symptoms.md` (fixed defects and the line that proves each fix) |

## Frame flow

`GLDisplay::render` (`game/graphics/pipelines/opengl.cpp:939`): `hdr::update`, `xr::begin_frame`
when a session runs, the renderer's `render`, `xr::end_frame`, then `hdr::present` or the GL
swap.

`OpenGLRenderer::render` (`OpenGLRenderer.cpp:1150`):

1. Mode decision: stereo when `stereo_enable` or an XR frame; a render scale below 1 sets the
   display size.
2. `setup_frame`: the render FBO is RGBA16F with the FP16 target, else RGBA8; one sample under
   temporal AA, the MSAA setting otherwise.
3. `UpscalerStage::decide` (`:1186`): FSR 3.1, DLSS or FSR 4 from the AA mode, the DLSS and
   FSR 4 fallbacks, and the release of the contexts the frame does not use.
4. Loader, then the buckets: `dispatch_buckets_stereo` (two eyes, `OpenGLRendererXr.cpp`) or
   `dispatch_buckets`, which calls `dispatch_buckets_jak1`; in XR the 2D layer canvases around
   them (`ui_layer_begin`/`ui_layer_finish`, `OpenGLRendererXr.cpp`).
5. `blit_display`, then `do_pcrtc_effects`: the final pass (`post_processing.frag`) tonemaps
   and encodes into the window, the HDR presenter's stand-in or the swapchain directly.
6. XR frame sequence (`xr_seq_capture`), screenshot (PNG, EXR of the FP16 scene, XR eye images
   via `save_xr_eyes` before the main PNG), debug windows.

`dispatch_buckets_jak1` (`:1682`), passes added around upstream's bucket loop:

| When | Pass |
|---|---|
| Before bucket 0 | stencil marks, normal and emissive draw buffers (AO, tile lights, bloom), camera priming, motion vectors `begin_frame`, upscaler jitter, motion blur setup, heightfield and canopy prepares, actor and world shadow map prepass |
| Start of `TFRAG_LEVEL0`/`1`, `SHRUB_NORMAL_LEVEL0`/`1` | background depth prepass (`run_prepass`, `:1932-1953`): the opaque depth of the run of buckets ahead, before they shade |
| `SHADOW` | replaced by the shadow maps and the landing disc when actor shadows run |
| bucket 30 (`SHRUB_GENERIC_LEVEL1`, last opaque background) | collision debug, vista neighbours, sun disc, stencil marks written |
| `TFRAG_ICE_LEVEL1` | vista transparent pass |
| `GENERIC_TFRAG_TEX_LEVEL1` | vista hi-Z build; water depth copy, then the held-back mid ocean |
| `GENERIC_PRIS` | GTAO, contact shadows, swamp mist |
| `OCEAN_NEAR` | water depth re-copy (swimmers), sea mist |
| `SPRITE` (end of scene) | tile lights, velocity resolve and motion blur, held HUD sprites, bloom, sun flare, then `UpscalerStage::run` (FSR 3.1, DLSS or FSR 4) into its display target; FSR 3.1's reactive mask around the bucket (`pre_sprite`/`post_sprite`) |
| `DEBUG` and later | HUD text and subtitles at display size; in XR they go to the 2D layer canvas |

Stereo: `dispatch_buckets_stereo` (`OpenGLRendererXr.cpp:160`) replays the same DMA chain per
eye with the per-eye camera in `m_render_state.eye`, skips per-frame side effects on the repeat
pass (`stereo_repeat_pass`), keeps one upscaler context, motion-vector history and AO history per
eye (`eye_slot`), copies each eye into a side-by-side buffer, tonemaps it once for the SDR
swapchains and crops each eye into its swapchain image.

## Settings flow

1. Files: `<settings dir>/sandover.gc` (flat screen) and `sandover-vr.gc`, never keys in
   `pc-settings.gc` (an unmodified OpenGOAL resets every setting on an unknown key). Written by
   `sandover-settings-write`, read by `sandover-settings-read` (unknown keys skipped, version
   migrations at `pckernel.gc:189-198`). Also read by `spatial.cpp` at start (audio output) and
   copied into bug reports (`bug_report.cpp:768`); `sandover/tools/sandover/settings.py` mirrors
   the format.
2. GOAL: `initialize` loads both profiles (`pckernel.gc:334-337`); the menu edits the active
   profile in `*pc-settings* sandover`; every confirm writes both files (`commit-to-file`).
3. Each frame `sandover-settings-update` (`main.gc:780`) picks the profile (VR while an OpenXR
   session runs), computes the effective settings (`sandover-effective!`: vanilla rendering is
   the boot state, children of an off switch fall back) and sends only the fields that changed
   since the last push, as `pc-set-*` forms. REPL and rig forms therefore stay until the menu
   changes that field; `sandover-settings-reapply` forgets what was pushed.
4. Bridge to engine: each `pc-set-*` writes `Gfx::g_global_settings` from the EE thread; the
   renderer reads it on its own thread.

## Data pipeline

| Tool | Reads | Writes | Consumed by |
|---|---|---|---|
| decompiler (`levels_extract`) | the disc image | fr3 levels, glb | everything below |
| `sandover-unbake` | glb vertex colours, mood tables | `<level>.unbake`, `*.canopy` | `Unbake.cpp`, `Canopy*.cpp` at level load |
| `sandover-census`, `sandover-lights census` / `bake` / `eco` / `launch` | decompiled entities, particles, `lights/entity_types.json` | `$SANDOVER_DATA/lights/*.json`, `WorldLights.inc`, `SelfLitTextures.inc`, `EcoLights.inc`, `LaunchLights.inc` | compiled into gk |
| `python -m sandover.lights.lava_volumes` | decompiled lava entities | `LavaVolumes.inc` | compiled into gk |
| `python -m sandover.sand_textures write` | the levels' collision pat materials | `SandTextures.inc` | compiled into gk |
| `sandover-materials`, `sandover-pat-census` | texture rips, collision pat | `materials.csv`, `emissive-actors.csv`, `pbr-maps/` | `Materials.cpp`, `EmissiveTable.cpp` at start |
| `sandover-mood-tables`, `sandover-levelinfo`, `sandover-caustics`, `sandover.inland_water` | `goal_src` mood and level info | `NeighboursMood.inc`, `LevelSky.inc`, `CausticMoods.inc`, `InlandWater.inc` | compiled into gk |
| `sandover-preset` | `presets/*.toml` | `SsaoPresets.inc`; `pc-set-*` forms for rig and play | gk, rig |
| `sandover-package build` | the above runtime files | the release zip | players |

Runtime data paths resolve `$SANDOVER_DATA` first, then `<project>/sandover/data`.
