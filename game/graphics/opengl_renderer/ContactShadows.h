#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/ScreenHistory.h"

class SsaoRenderer;

/*!
 * Screen-space contact shadows (Jak 1): a short ray per pixel toward the shadow light (the sun,
 * or the moon at night: the world sun map's light, WorldShadows.h) through the SSAO depth copy,
 * after the character buckets (56). A hit darkens only the direct share of the light that the
 * shadow maps left at the pixel, so the pass composes with them and never lightens:
 * colour x (1 - k x hit x r), r the share of the pixel's colour that is still direct light
 * (the fitted sun share at the surface's N.L, after the world and actor sun maps), k the
 * strength. Sky and translucent pixels (stencil bit 7) are skipped; emissive pixels and fog fade
 * it as they fade the SSAO.
 *
 * Three passes: the raw term (16 or 8 dithered taps, rotated per frame) into an R16F target, a
 * temporal blend into its own reprojected history (ScreenHistory: disocclusion by depth, a 3x3
 * neighbourhood clamp, reset on cuts), then a depth-aware 3x3 that multiplies the frame. The term
 * converges before the upscaler, which does not denoise it. Needs SSAO (its depth copy and
 * normal target) and the actor and world sun maps, and runs only on the temporal AA path (FSR 3,
 * DLSS, FSR 4); under any other mode it is forced off (logged once).
 * ShadowOption::CONTACT 1 moderate, 2 strong; CONTACT_VIEW debug views. Off: no GL work.
 */
class ContactShadows {
 public:
  ContactShadows() = default;
  ~ContactShadows();
  ContactShadows(const ContactShadows&) = delete;
  ContactShadows& operator=(const ContactShadows&) = delete;

  /// Darken this dispatch's frame; call right after SsaoRenderer::render (bucket 56).
  /// `temporal`: a temporal AA pass (FSR 3, DLSS, FSR 4) resolves this frame; without one
  /// nothing draws.
  void render(SharedRenderState* render_state,
              ScopedProfilerNode& prof,
              SsaoRenderer& ssao,
              bool temporal);
  /// Free the raw target and the history (off, or no temporal AA).
  void release();

  /// Ray parameters of a strength (1 moderate, 2 strong) at shadow quality `quality` (0-2).
  struct Params {
    int steps;          // ray taps
    float length_m;     // ray length near the camera
    float thickness_m;  // a depth sample this far in front of the ray hides it
    float strength;     // share of the remaining direct light a full hit removes
    float screen_cap;   // longest ray on screen, share of the frame height
    int history;        // frames the temporal pass accumulates at most
  };
  static Params params(int level, int quality);

 private:
  void ensure_raw(int w, int h);
  void release_raw();
  GLuint m_vao = 0;
  GLuint m_raw_fbo = 0, m_raw_tex = 0;  // R16F raw term, render size
  int m_raw_w = 0, m_raw_h = 0;
  ScreenHistory m_history;  // r term, g view depth (m), b frames accumulated
  bool m_warned = false;
};
