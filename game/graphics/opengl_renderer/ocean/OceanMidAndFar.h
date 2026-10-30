#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/DirectRenderer.h"
#include "game/graphics/opengl_renderer/ocean/OceanEnvmap.h"
#include "game/graphics/opengl_renderer/ocean/OceanMid.h"
#include "game/graphics/opengl_renderer/ocean/OceanTexture.h"
#include "game/graphics/opengl_renderer/opengl_utils.h"

/*!
 * OceanMidAndFar is the handler for the first ocean bucket.
 * This bucket runs three renderers:
 * - ocean-texture (handled by the OceanTexture C++ class)
 * - ocean-far (handled by this class, it's very simple)
 * - ocean-mid (handled by the C++ OceanMid class)
 */
class OceanMidAndFar : public BucketRenderer {
 public:
  OceanMidAndFar(const std::string& name, int my_id);
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  void render_jak1(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void render_jak2(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof);
  void draw_debug_window() override;
  void init_textures(TexturePool& pool, GameVersion version) override;
  /// Draws the mid ocean kept back while the water shading is on (Jak 1).
  void flush_mid_deferred(SharedRenderState* render_state, ScopedProfilerNode& prof) {
    m_mid_renderer.flush_deferred(render_state, prof);
  }
  bool mid_pending() const { return m_mid_renderer.mid_pending(); }
  /// The game drew an ocean this frame: the near ocean only comes with this bucket (draw-ocean).
  bool drawn_in_frame(u64 frame_idx) const { return m_drawn_frame == frame_idx; }

 private:
  void handle_ocean_far(DmaFollower& dma,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);
  void handle_ocean_mid(DmaFollower& dma,
                        SharedRenderState* render_state,
                        ScopedProfilerNode& prof);

  DirectRenderer m_direct;
  OceanEnvmap m_envmap_renderer;
  OceanTexture m_texture_renderer;
  OceanMid m_mid_renderer;
  u64 m_drawn_frame = UINT64_MAX;
};
