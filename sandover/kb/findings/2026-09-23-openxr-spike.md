---
title: Can the engine run an OpenXR session with the GL binding and submit the stereo eyes?
date: 2026-09-23
issue: none
status: open
sha: branch openxr
---
## Answer (≤5 lines)
Partly verified. The OpenXR module is complete: the vendored loader, instance, system, session on the window's WGL context, LOCAL space, per-eye swapchains, the frame loop, and the eye copy. With no headset attached, the furthest verified step is `xrCreateInstance` on SteamVR 2.17.10 with `XR_KHR_opengl_enable`. `xrGetSystem(HMD)` returns `XR_ERROR_FORM_FACTOR_UNAVAILABLE`, so the engine stays in `NO_SYSTEM` and retries every 2 s. The window renders normally throughout. Nothing from the session onward has been run yet: swapchain formats, frame loop, eye copy, timing. That needs the SteamVR null driver (a change to the SteamVR settings, see `howto/vr.md`) or a headset.

## Evidence
Runs used the worktree `gk` against the primary checkout's data, on a rig slot at 2560×1440. Logs and window grabs are in `$SANDOVER_DATA/openxr/`.

| Run | Setup | XR log | Window |
|---|---|---|---|
| `xroff` | no `--xr` | no `[xr]` lines: no OpenXR calls | normal |
| `noruntime`, `noruntime2` | `--xr`, `XR_RUNTIME_JSON` pointed at a missing file | `no OpenXR runtime available: XR_ERROR_RUNTIME_UNAVAILABLE`, `state OFF -> NO_RUNTIME`, retried every 5 s | normal (`noruntime-1.png`) |
| `steamvr` | `--xr`, ActiveRuntime = SteamVR, no headset | active runtime logged, 41 extensions, `instance created, runtime: SteamVR/OpenXR 2.17.10`, `state OFF -> NO_SYSTEM`, `xrGetSystem(HMD): XR_ERROR_FORM_FACTOR_UNAVAILABLE` | normal (`steamvr-1.png`) |

Runtime facts (SteamVR 2.17.10, from the run):
- `ActiveRuntime` is SteamVR's `steamxr_win64.json`. The Meta and Virtual Desktop runtimes are still registered as available, and Virtual Desktop's implicit `oculus-compatibility` layer is still registered (OpenXR environment check still holds).
- Offered extensions include `XR_KHR_opengl_enable`, `XR_KHR_D3D11_enable`, `XR_KHR_D3D12_enable`, `XR_KHR_vulkan_enable2`, `XR_KHR_composition_layer_depth`, `XR_KHR_visibility_mask`, `XR_FB_display_refresh_rate`, `XR_EXT_local_floor`, `XR_META_recommended_layer_resolution`, `XR_MND_headless` and `XR_EXT_view_configuration_views_change`.
- The first `xrCreateInstance` took about 17 s, because it starts SteamVR. It ran on the render thread, so the window froze for that long. After the game quit, no SteamVR process was left running.
- Swapchain formats: **not observed**. They can only be listed once a session exists. The engine logs them in runtime order (`[xr] swapchain formats, runtime order: ...`) and prefers `GL_SRGB8_ALPHA8`, then `GL_RGBA8`, then `GL_RGBA16F`. The tooling evaluation note says SteamVR's GL path accepts `GL_RGBA8`/`GL_SRGB8_ALPHA8`. The null driver's defaults are 1512×1680 per eye at 90 Hz (its `default.vrsettings`).

Code (branch `openxr`):
- `third-party/openxr`: OpenXR-SDK `release-1.1.63`, static loader. llvm-rc cannot parse `loader.rc`, so the root `CMakeLists.txt` excludes it without editing the vendored files.
- `game/graphics/xr/OpenXr.{h,cpp}`: the state machine is `OFF → NO_RUNTIME | NO_SYSTEM → IDLE → RUNNING`. It also has `FAILED`, with a 10 s retry. The session events handled are READY (begin), STOPPING (end), EXITING and LOSS_PENDING (drop the session and retry), and instance loss.
- `pipelines/opengl.cpp`: `xr::update` and `begin_frame` run before the game frame, and `end_frame` runs after it. While the session runs, the eye size overrides `game_res`. Window vsync and the frame limiter are off while `xrWaitFrame` paces the loop.
- `OpenGLRenderer::dispatch_buckets_stereo`: the eye offset is `-head_x × 4096 / world_scale` (OpenXR +x right, camera +x screen left). The IPD comes from the located views; the fixed-IPD path stays as the fallback. Each eye is blitted into its acquired image, shifted so the game's centred frustum sits on the runtime's forward direction. The layer is submitted with the located poses and `XrFovf` in LOCAL.

## Gaps
| Gap | Why it matters | Where it goes |
|---|---|---|
| Session path unverified | Formats, sRGB passthrough, the shift sign and frame timing are all untested | Null driver or headset run (howto) |
| Head pose not applied to the game camera | The image is head-locked while it is submitted in LOCAL, so the compositor reprojects it, and it may swim | Union camera and diorama anchor |
| FOV and aspect | GOAL projects with its own FOV and aspect into the runtime's eye rectangle. Scale is wrong and the eye aspect (~0.9) squashes a 16:9 projection | Drive the GOAL FOV and aspect from `XrFovf` |
| HUD | Drawn in both eyes at zero parallax | Quad layer |
| Input | Gamepad only, unchanged; no XR actions | By design (ADR-0004) |
| Seated origin | LOCAL has no recentre handling; `XR_EXT_local_floor` is offered but unused | later VR work |
| World scale | `stereo_world_scale` still applies; no preset | later VR work |
| Instance creation blocks | About 17 s on first start | Create the instance on a worker thread |
| Pacing | The game produces 60 Hz; the loop runs at the runtime rate and submits no layer when no new chain arrived | Measure in the first session run |

## Implications for the design
- The planned integration table holds as written up to `xrGetSystem`. SteamVR also offers `XR_KHR_D3D11_enable`, so the interop contingency stays open.
- Run instance creation off the render thread before this ships, because SteamVR start-up alone is longer than any frame budget.

## Follow-ups
- Human: enable the null driver (or connect the headset) and run `gk --xr`. The log then gives the swapchain formats, recommended size and timing, and should be recorded in `facts/`.
- Check the projection-centre shift sign on a headset: a near object should fuse without vertical or horizontal strain.
