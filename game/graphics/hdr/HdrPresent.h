#pragma once

/*!
 * @file HdrPresent.h
 * HDR window output for the OpenGL renderer on Windows. The GL context keeps rendering; the
 * window is presented through a DXGI flip-model swapchain (HDR10 PQ or scRGB FP16) whose image
 * GL writes through WGL_NV_DX_interop2. The swapchain belongs to a disabled child window that
 * covers the SDL window's client area, because the GL driver may already own the one flip chain
 * an HWND allows on the SDL window itself. While it runs, everything that used to draw to the
 * window draws to Gfx::window_fbo() instead (linear scRGB, 1.0 = 80 nits), and a final pass
 * encodes that into the swapchain format. Off, nothing here is created and the window presents
 * with SDL_GL_SwapWindow as before. Other platforms: update() is a no-op.
 */

#include <string>

struct SDL_Window;

namespace hdr {

/*!
 * Called once per display frame on the render thread with the GL context current, before
 * anything draws to the window. Follows Gfx::g_global_settings.hdr_output (when `allowed`),
 * resizes to the window's pixel size and fills Gfx::g_hdr_present.
 */
void update(SDL_Window* window, bool allowed, int fb_w, int fb_h);

/*!
 * Encodes the frame into the swapchain and presents it. Returns false when HDR output is not
 * running, in which case the caller swaps the GL window as usual.
 */
bool present(int vsync);

/*!
 * Before the frame renders: whether the final pass may write the swapchain image itself this
 * frame (perf-opt bit 16). Not while the splash or last frame's overlays share the window.
 */
void allow_direct(bool splash);

struct DirectTarget {
  unsigned fbo = 0;    // 0: draw to Gfx::window_fbo() as usual
  int h = 0;           // rows run top first: flip y and mirror the viewport
  bool pq = false;     // encode PQ (BT.2020, 10000 nits) after the tonemap; else linear scRGB
  bool check = false;  // also draw the stand-in as usual (SANDOVER_HDR_DIRECT_CHECK)
};

/*!
 * Called by the final pass. When the direct path is on for this frame, locks this frame's
 * interop image and returns its framebuffer; present() then only unlocks and flips it.
 */
DirectTarget begin_direct(int fb_w, int fb_h, bool eligible);

/*!
 * After the overlays (imgui): whether any drew over the window. One that appears while the
 * direct path is on shows from the next frame.
 */
void note_overlay(bool drew);

/*!
 * For the present log line: window mode, swapchain format, colour space and composition.
 */
std::string present_tag();

/*!
 * One-line status: display name, HDR state, format and the effective calibration values.
 */
std::string status_line();

/*!
 * Releases everything and restores the Windows HDR state if this module switched it on.
 */
void shutdown();

}  // namespace hdr
