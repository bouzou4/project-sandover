#include "Merc2BucketRenderer.h"

#include "game/graphics/opengl_renderer/StencilMarks.h"

Merc2BucketRenderer::Merc2BucketRenderer(const std::string& name,
                                         int my_id,
                                         std::shared_ptr<Merc2> merc)
    : BucketRenderer(name, my_id), m_renderer(merc) {}

void Merc2BucketRenderer::render(DmaFollower& dma,
                                 SharedRenderState* render_state,
                                 ScopedProfilerNode& prof) {
  // skip if disabled
  if (!m_enabled) {
    while (dma.current_tag_offset() != render_state->next_bucket) {
      dma.read_and_advance();
    }
    return;
  }

  render_state->merc_props = render_state->version == GameVersion::Jak1 &&
                             (m_my_id == (int)jak1::BucketId::MERC_TFRAG_TEX_LEVEL0 ||
                              m_my_id == (int)jak1::BucketId::MERC_TFRAG_TEX_LEVEL1);
  m_renderer->set_bucket(m_my_id);
  m_renderer->render(dma, render_state, prof, &m_debug_stats);
  if (render_state->merc_props) {
    stencil_marks::props(render_state, m_my_id, true);
  }
  render_state->merc_props = false;

  m_empty = m_debug_stats.num_predicted_draws == 0;
}

void Merc2BucketRenderer::draw_debug_window() {
  m_renderer->draw_debug_window(&m_debug_stats);
}

bool Merc2BucketRenderer::empty() const {
  return m_empty;
}