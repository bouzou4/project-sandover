#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/Fbo.h"

/*!
 * Render scale (Jak 1): the scene renders below the display size and is upscaled once it is
 * complete (after the sprite bucket, bloom and SMAA), before the debug, HUD text and subtitle
 * buckets, which then draw at display size into the display target. The upscale is bilinear or
 * AMD FidelityFX FSR 1 (EASU, then RCAS; shaders/ffx unmodified), on the display-encoded scene
 * buffer, so the tonemap and the HDR output encode still run after it, in the final pass, which
 * reads display() instead of the render framebuffer. Nothing here runs at render scale 1.
 */
class Upscaler {
 public:
  Upscaler() = default;
  ~Upscaler();
  Upscaler(const Upscaler&) = delete;
  Upscaler& operator=(const Upscaler&) = delete;

  /// Upscale render_state->render_fb to display_w x display_h and make the display target the
  /// render framebuffer for the buckets that follow (bound, with its viewport).
  void run(SharedRenderState* render_state, ScopedProfilerNode& prof, int display_w, int display_h);
  /// The display-size target the last run() drew; the text buckets draw into it.
  Fbo* display() { return &m_display; }
  /// Free the render- and display-size targets; no-op when none are allocated.
  void release_targets() { free_targets(); }

 private:
  void ensure(int rw, int rh, int dw, int dh, bool hdr, bool fsr);
  void free_targets();

  int m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
  bool m_hdr = false;
  Fbo m_src;      // render size, single-sample copy of the scene
  Fbo m_easu;     // display size, EASU output (RCAS input); FSR only
  Fbo m_display;  // display size, with depth-stencil for the text buckets
  GLuint m_vao = 0;
};
