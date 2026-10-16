---
title: HDR present path for the OpenGL renderer on Windows
date: 2026-09-25
issue: none
status: inferred
sha: branch p8/hdr-output (base d2c7ab013)
---
## Answer (≤5 lines)
HDR output uses a DXGI flip-model swapchain on the game's own window (scRGB `R16G16B16A16_FLOAT`
+ `G10_NONE_P709`, or PQ `R10G10B10A2` + `G2084_NONE_P2020`), with GL rendering into a D3D11
texture shared through `WGL_NV_DX_interop2`, copied to the back buffer and presented by DXGI.
Neither SDL3's GL path nor a WGL colour-space extension can put an OpenGL 4.3 window into
scRGB or PQ on Windows. Compiled, not yet seen on an HDR display (status stays inferred).

## Options considered

| Path | Verdict | Why |
|---|---|---|
| SDL3 GL window | no | SDL3's HDR support (`SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN`, SDR white, headroom) is read-only display info plus colour-space output for `SDL_Renderer` (D3D11/12, Vulkan). For GL it only offers `SDL_GL_FLOATBUFFERS` (`third-party/SDL/src/video/windows/SDL_windowsopengl.c:658`), a `WGL_ARB_pixel_format_float` pixel format with no colour-space tag. |
| FP16 WGL pixel format | no | The format is fixed at window creation (SDL sets it before the window exists), and whether DWM treats an FP16 GL back buffer as scRGB is driver behaviour no vendor documents as a windowed path (inferred from vendor HDR guides, which describe it for exclusive fullscreen at best). Not dependable. |
| `WGL_EXT_colorspace` | no | Only selects sRGB vs linear encoding of an 8-bit default framebuffer; it has no HDR colour spaces. |
| DXGI swapchain + `WGL_NV_DX_interop2` | **chosen** | DXGI is the only API through which Windows accepts an HDR colour space for a window (`IDXGISwapChain3::SetColorSpace1`). The interop extension is shipped by NVIDIA, AMD and Intel on Windows, works with a GL 4.3 core context, and the design already planned it (the design item "HDR presenter with GL interop"). Works windowed and borderless. |
| `GL_EXT_memory_object_win32` | not needed | Also shares D3D memory with GL, but needs explicit semaphores for sync; the NV interop lock/unlock is simpler for one texture per frame. |

## How it is wired
How to use it: `sandover/kb/howto/hdr-output.md`.

- `game/graphics/hdr/HdrPresent.cpp`: D3D11 device on the adapter that owns the window's
  monitor, swapchain on the SDL window's HWND, one shared texture in the swapchain format.
- While it runs, everything that drew to the window (`glBindFramebuffer(..., 0)` in the final
  pass, the letterbox clear, splash, ImGui) draws to an RGBA16F GL framebuffer instead
  (`Gfx::window_fbo()`); `present()` encodes that into the shared texture (pass-through for
  scRGB, BT.709 to BT.2020 + PQ for HDR10), unlocks it, `CopyResource`s it into the back buffer
  and calls `Present`. The copy instead of registering the back buffer itself keeps the
  flip-model buffer rotation out of GL.
- GL never calls `SwapBuffers` while DXGI owns presentation; off, `SDL_GL_SwapWindow` runs as
  before and none of the D3D objects exist.
- If Windows HDR is off for the display, the game logs it and stays SDR. Opt-in only (option 7,
  default off): the app switches it on (`DisplayConfigSetDeviceInfo` with
  `DISPLAYCONFIG_SET_ADVANCED_COLOR_STATE`) and restores it when HDR output stops or the game
  exits.
- Before this change every window was an 8-bit SDR swapchain: the FP16 target (#163) was
  tonemapped to [0,1] and never reached the display above SDR white.

## Calibration seed
- Display profile: `ColorProfileGetDisplayDefault` (mscms, Windows 11) with the extended display
  colour mode subtype returns the advanced-colour profile, which is where the Windows HDR
  Calibration app's result lives; `GetICMProfileW` is the fallback. The `MHC2` tag gives min and
  max luminance (s15Fixed16 at tag offsets 12 and 16) and a 3x4 matrix. Layout from Microsoft's
  "ICC profile behavior with Advanced Color" documentation; not yet checked against the maintainer's
  profile, the log prints what was parsed.
- DXGI `GetDesc1` gives min/max/full-frame luminance, primaries and the current colour space.
- The MHC2 matrix and the display primaries are logged but not applied: in scRGB Windows'
  compositor applies the profile itself (advanced colour management), and PQ is sent as
  BT.2020 for the display to map.

## Not verified
- Behaviour on the maintainer's display (the lead boots the SDR path only; HDR is tested by the
  human). Specifically: interop registration of `R10G10B10A2`, handing the window back to GL
  after turning HDR output off at run time, and the OS switch round trip.
