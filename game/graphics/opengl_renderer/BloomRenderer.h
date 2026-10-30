#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/SsaoDepth.h"

/*!
 * Emissive bloom (Jak 1): bright pass = emissive mask E x the lit colour above a threshold,
 * at half resolution, a separable Gaussian blur, then an additive composite into the render
 * framebuffer (RGB only; destination alpha is left alone). E comes from SsaoRenderer's
 * emissive mask target: background geometry with .unbake v2 data, additive sprites
 * (particles) and the sprite and merc textures listed in the emissive table (EmissiveTable.h);
 * HUD sprites clear it. The copies are RGBA16F under the FP16 target, so colour above 1 reaches
 * the bright pass. Nothing here runs while Gfx::g_global_settings.bloom_enable is off.
 */
class BloomRenderer {
 public:
  BloomRenderer() = default;
  ~BloomRenderer();
  BloomRenderer(const BloomRenderer&) = delete;
  BloomRenderer& operator=(const BloomRenderer&) = delete;

  /// Run the three passes; `em_tex` is the emissive mask, `em_samples` its sample count.
  void render(SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              GLuint em_tex,
              int em_samples);
  void draw_debug_window();

 private:
  struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
  };
  void ensure_targets(int w, int h, bool hdr);
  void free_targets();

  int m_w = 0, m_h = 0;
  bool m_hdr = false;
  Target m_resolve;  // full-res single-sample copy of the frame
  Target m_half[2];  // half-res bright pass and blur ping-pong
  GLuint m_vao = 0;
  GpuTimer m_timer{"bloom-total"};
};
