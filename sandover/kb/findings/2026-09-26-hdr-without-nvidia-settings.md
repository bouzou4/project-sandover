---
title: HDR output without asking players to change NVIDIA (or AMD) present settings
date: 2026-09-26
issue: none
status: inferred
sha: 061cd492b
---
## Answer (≤5 lines)
Present from a disabled child window that covers the SDL window's client area, not from the
SDL window itself. Windows allows one flip-model swapchain per HWND; with "Prefer layered on
DXGI swapchain" (and on AMD whenever Windows HDR is on) the GL driver already holds it, hence
`E_ACCESSDENIED`. A child HWND has no GL swapchain, so ours never collides. A shipping OpenGL HDR
mod uses exactly this. Detection, NVAPI profile writes, FP16 GL pixel formats and a Vulkan blit are all worse.

## The failure, explained
- Microsoft: "you can associate only one flip presentation model swap chain at a time with an
  HWND" (CreateSwapChainForHwnd remarks). The flip-model guide adds that the flip model cannot
  have "multiple different APIs producing content, which all layer together into the same HWND".
  https://learn.microsoft.com/en-us/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforhwnd
  https://learn.microsoft.com/en-us/windows/win32/direct3ddxgi/for-best-performance--use-dxgi-flip-model
- The NVIDIA control panel option layers GL/Vulkan presentation "on top of a driver created
  DXGI swapchain" on the app's window (driver 526.61+), so by the time `start()` runs
  (`game/graphics/hdr/HdrPresent.cpp:613@061cd492b`) the HWND already has its one flip chain.
  https://community.khronos.org/t/vulkan-present-mode-preferred-native-vs-layered-on-dxgi-swapchain/112055
- **AMD does the same without any setting**: since Adrenalin 23.7.1 the driver presents every
  OpenGL and Vulkan app through a DXGI swapchain when Windows HDR is on (minus a hardcoded
  exclusion list), or when OpenGL triple buffering is on (PCGamingWiki, HDR glossary,
  "OpenGL/Vulkan"). So on AMD our same-HWND path likely fails exactly when HDR is on. Inferred,
  not tested on AMD. https://www.pcgamingwiki.com/wiki/Glossary:High_dynamic_range_(HDR)
- Third-party tools flip the NVIDIA option without the player knowing: Sunshine writes
  `OGL_CPL_PREFER_DXPRESENT = PREFER_ENABLED` into the **base** driver profile at start
  (its log line: "Changed OGL_CPL_PREFER_DXPRESENT to OGL_CPL_PREFER_DXPRESENT_PREFER_ENABLED
  for base profile"). Special K's Vulkan bridge sets it per game. "Ask the player to change a
  setting" therefore also breaks when a tool they run changes it back.
  https://github.com/LizardByte/Sunshine/issues/1771

## Options, ranked

| # | Option | Verdict | Effort | Risk |
|---|---|---|---|---|
| 1 | Swapchain on a child HWND | **do first** | small | low to medium |
| 2 | Detect the layered mode | diagnostics only | tiny | none |
| 3 | NVAPI DRS app profile write | last resort, likely not needed | medium | high |
| 4 | FP16/10-bit GL default framebuffer | no | small to try | high |
| 5 | Vulkan final blit | no | large | medium |

### 1. Child HWND presenter (recommended, always on, not only on failure)
- Prior art: VintageStoryHDR (an OpenGL game mod, same WGL_NV_DX_interop2 + scRGB flip-model
  design as ours) presents from a `STATIC` child created with `WS_CHILD | WS_VISIBLE |
  WS_DISABLED` covering the parent's client area; disabled, so mouse and keyboard still reach the
  game. Its findings file lists "swapchain on the game window itself" as a dead end: "GL has
  already presented to it; flip-model DXGI wants a window nothing else presents to". Verified by
  its author on an NVIDIA RTX 30-series card, driver 616.92, Windows 11, 4K HDR.
  https://github.com/cjohnsto-nz/VintageStoryHDR (`docs/findings.md`,
  `src/VintageStoryHDR/Rendering/HdrPresenter.cs` at 5030ca6f)
  Its docs do not say whether it was tried with the layered option on; that is our one test.
- Changes in `HdrPresent.cpp`: create the child in `start()` (after `query_display`), pass the
  child to `CreateSwapChainForHwnd`, keep the parent for `MonitorFromWindow`,
  `MakeWindowAssociation` and the OS-HDR switch; `SetWindowPos` the child next to the existing
  `ResizeBuffers` (`:749`); `DestroyWindow` it in `stop()` after the swapchain is released
  (flip-chain deferred destruction: `Flush()` first). Roughly 60 to 100 lines.
- Why it fixes the problem rather than hiding it: the parent keeps whatever swapchain the driver
  gave it (native, layered, or AMD's DXGI); we just stop calling `SwapBuffers` on it while the
  child covers it. No HWND ever has two flip chains.
- Bonus: turning HDR off mid-session no longer hands a window from DXGI back to GL (the known
  "window may stay black, restart" limit in `kb/howto/hdr-output.md`): destroy the child and
  the parent's untouched GL chain resumes.
- Risks to check in the one confirmation run:
  - Create the child on the thread that owns the SDL window (the one pumping SDL events);
    a child owned by another thread silently attaches the two input queues.
  - Latency: a child-window swapchain may not get independent flip / MPO where a top-level
    one would (not documented either way). Record PresentMon's present mode once; do not tune.
  - RTX HDR or Auto HDR on top: both target SDR swapchains; an FP16/HDR10 chain with an HDR
    colour space should be left alone (inferred, not documented). The parent's layered chain
    stops presenting, so there is nothing for them to convert.
  - Overlays (Steam, Discord, capture tools) hook whichever DXGI chain presents; they now see
    ours. Same as today in native mode.

### 2. Detecting the layered mode
- No public API enumerates the swapchains bound to an HWND. The robust signal is the one we
  already get: `CreateSwapChainForHwnd` returning `E_ACCESSDENIED` (0x80070005) on the SDL
  window, whose documented cause is "HWND already has a flip chain". It is vendor-neutral
  (NVIDIA option, AMD HDR-on present, a hooking overlay).
- NVAPI can read the setting: `OGL_CPL_PREFER_DXPRESENT_ID = 0x20D690F8`, values
  `PREFER_DISABLED = 0` (native), `PREFER_ENABLED = 1` (layered), `AUTO = 2` (default)
  (`NvApiDriverSettings.h`, https://github.com/NVIDIA/nvapi). Reading via `NvAPI_DRS_*` is
  cheap, but `AUTO` lets the driver choose, and it says nothing about AMD. Use it only to name
  the cause in the log. With option 1 no detection is needed to pick a path.

### 3. Writing the NVIDIA profile programmatically (last resort)
- Possible: create an application profile for the game executable and set
  `OGL_CPL_PREFER_DXPRESENT = PREFER_DISABLED` (`NvAPI_DRS_CreateSession`, `LoadSettings`,
  `FindProfileByName`/`CreateProfile`, `CreateApplication`, `SetSetting`, `SaveSettings`).
  Sunshine does exactly this sequence (`src/platform/windows/nvprefs/driver_settings.cpp`,
  https://github.com/LizardByte/Sunshine), with an undo file so it can restore the old value.
- Against: it changes persistent driver state for a setting the player chose on purpose; it
  takes effect only at the next launch (the driver reads the profile at context creation); it
  likely needs elevation (NVAPI marks many DRS-adjacent calls "requires Administrator
  privileges"; Sunshine runs as a service; not confirmed for `SaveSettings`); and it does
  nothing for AMD. Only worth it if option 1 fails on NVIDIA.

### 4. Letting the layered chain present an FP16/10-bit GL framebuffer
- SDL3 (vendored 3.4.10) cannot tag a GL window as HDR: `SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`
  and the headroom/white-level properties are read-only display state set in
  `SDL_video.c:1187-1189`; for GL it only has `SDL_GL_FLOATBUFFERS`
  (`SDL_windowsopengl.c:658`), a `WGL_ARB_pixel_format_float` format chosen before the window
  exists (so toggling HDR means recreating the window).
- In native mode an FP16 GL pixel format reportedly reaches the display as scRGB on NVIDIA
  (Khronos forum: values to about 25.0 shown as HDR) while AMD clamps it to 1.0 on the same
  display. https://community.khronos.org/t/amd-opengl-and-hdr-display-problem/108379 ,
  https://github.com/glfw/glfw/issues/1907
- Nothing from NVIDIA documents whether the layered chain keeps an FP16 or 10-bit format or sets
  an HDR colour space, and there is no PQ/HDR10 or colour-space control from GL. It would be a
  second, NVIDIA-only path that can silently turn SDR. Not worth building.

### 5. Vulkan for the final blit
- A Vulkan swapchain is also a flip chain on the HWND (on NVIDIA in layered mode, literally a
  DXGI one), so on the SDL window it would hit the same one-chain rule and need its own child
  window anyway. It adds a Vulkan device, `GL_EXT_memory_object_win32` plus semaphores, and
  `VK_EXT_swapchain_colorspace`/`VK_EXT_hdr_metadata` on top of what D3D11 already does.
  Upstream OpenGOAL has no Vulkan renderer to reuse. Strictly dominated by option 1.

## What option 1 still cannot cover
- Windows HDR off for the display (unless the player opts in with option 7).
- Hybrid laptops where GL runs on the discrete GPU but the panel belongs to the integrated one:
  the D3D device is created on the monitor's adapter, and `wglDXOpenDeviceNV` needs the same
  adapter as GL. Not covered today either; untested.
- Tools that inject into and replace the process's DXGI presentation (some overlays, Special K).

## Implications for the design
- Make the child window the only path; drop the "set the present method to Auto" advice from
  `kb/howto/hdr-output.md` once the confirmation run passes.
- Keep the `E_ACCESSDENIED` log line, and if the child path ever fails with it, log the NVAPI
  setting value as the likely cause.

## Follow-ups
- Implement option 1 (engine change: a second reader before merge).
- One confirmation run on the HDR display: NVIDIA option set to layered, HDR
  output on, scRGB and PQ; toggle HDR output off and on; check mouse, keyboard and window resize.
- If an AMD machine is ever available: same run with Windows HDR on.
