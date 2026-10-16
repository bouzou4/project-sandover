#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * SMAA 1x (Jimenez et al., shaders/SMAA.hlsl) on the Jak 1 scene, after the SPRITE bucket and
 * bloom and before the debug, HUD text and subtitle buckets, so text stays sharp. The render
 * framebuffer's colour is copied (resolved, at 2 samples) to a single-sample texture; colour edges
 * are found on a perceptual scale (the buffer is display-encoded; FP16 headroom above 1.0 is
 * log-compressed), blend weights come from the area and search textures, and the neighbourhood
 * blend writes RGB back into the render framebuffer (destination alpha untouched). The tonemap
 * and the HDR output encode run afterwards in the final pass. Nothing here runs unless
 * Gfx::smaa_on().
 */
class SmaaRenderer {
 public:
  SmaaRenderer() = default;
  ~SmaaRenderer();
  SmaaRenderer(const SmaaRenderer&) = delete;
  SmaaRenderer& operator=(const SmaaRenderer&) = delete;

  /// Antialias the colour of render_state->render_fb in place.
  void render(SharedRenderState* render_state, ScopedProfilerNode& prof);
  /// Free the frame-sized targets (the lookup tables stay); no-op when none are allocated.
  void release_targets() { free_targets(); }

 private:
  struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
  };
  void ensure(int w, int h, bool hdr);
  void free_targets();

  int m_w = 0, m_h = 0;
  bool m_hdr = false;
  Target m_src;      // single-sample copy of the frame, same format as the render target
  Target m_edges;    // RG8
  Target m_weights;  // RGBA8
  GLuint m_area = 0, m_search = 0;
  GLuint m_vao = 0;
};
