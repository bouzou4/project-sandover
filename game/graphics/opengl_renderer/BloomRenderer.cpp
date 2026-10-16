#include "BloomRenderer.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/EmissiveTable.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

#include "third-party/imgui/imgui.h"

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

namespace {
void make_target(GLuint& fbo, GLuint& tex, int w, int h, bool hdr) {
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, hdr ? GL_RGBA16F : GL_RGBA8, w, h, 0, GL_RGBA,
               hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  GLenum draw = GL_COLOR_ATTACHMENT0;
  glDrawBuffers(1, &draw);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
    lg::error("bloom: incomplete framebuffer ({}x{})", w, h);
  }
}

}  // namespace

BloomRenderer::~BloomRenderer() {
  free_targets();
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void BloomRenderer::free_targets() {
  for (Target* t : {&m_resolve, &m_half[0], &m_half[1]}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    *t = Target();
  }
  m_w = m_h = 0;
}

void BloomRenderer::ensure_targets(int w, int h, bool hdr) {
  if (w == m_w && h == m_h && hdr == m_hdr) {
    return;
  }
  free_targets();
  make_target(m_resolve.fbo, m_resolve.tex, w, h, hdr);
  for (auto& t : m_half) {
    make_target(t.fbo, t.tex, std::max(1, w / 2), std::max(1, h / 2), hdr);
  }
  glBindTexture(GL_TEXTURE_2D, 0);
  m_w = w;
  m_h = h;
  m_hdr = hdr;
  lg::info("bloom: allocated targets {}x{} {}", w, h, hdr ? "RGBA16F" : "RGBA8");
}

void BloomRenderer::render(SharedRenderState* rs,
                           ScopedProfilerNode& prof,
                           GLuint em_tex,
                           int em_samples) {
  const auto& gs = Gfx::g_global_settings;
  if (!em_tex) {
    return;
  }
  const bool timing = gs.ssao_gpu_timing;
  static glq::SavedState saved("bloom", kGlq, 0b011, 0b010);
  saved.capture();
  if (timing) {
    m_timer.begin();
  }
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  const int hw = std::max(1, w / 2), hh = std::max(1, h / 2);
  // float copies under the FP16 target, so the bright pass keeps colour above 1
  ensure_targets(w, h, rs->hdr_target);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

  // resolve the frame (read buffer 0 of the render framebuffer)
  glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
  glReadBuffer(GL_COLOR_ATTACHMENT0);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve.fbo);
  glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);

  // bright pass at half resolution
  {
    glBindFramebuffer(GL_FRAMEBUFFER, m_half[0].fbo);
    glViewport(0, 0, hw, hh);
    auto& sh = rs->shaders[ShaderId::BLOOM_BRIGHT];
    sh.activate();
    perf::set_opt_uniform(sh.id());
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "color_tex"), 0);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "em_ss"), 1);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "em_ms"), 2);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "em_samples"), em_samples);
    glUniform1f(GLQ_LOC(kGlq, sh.id(), "threshold"), gs.bloom_threshold);
    glUniform2f(GLQ_LOC(kGlq, sh.id(), "em_ramp"), gs.ssao_emissive_lo,
                std::max(gs.ssao_emissive_hi, gs.ssao_emissive_lo + 1e-3f));
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_resolve.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, em_samples > 1 ? 0 : em_tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, em_samples > 1 ? em_tex : 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindTexture(GL_TEXTURE_2D_MULTISAMPLE, 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
  }

  // separable Gaussian, x then y
  {
    auto& sh = rs->shaders[ShaderId::BLOOM_BLUR];
    sh.activate();
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "src"), 0);
    // radius in render pixels: scaled with the render scale so it keeps its size on screen
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "radius"),
                std::clamp((int)(gs.bloom_radius * rs->render_scale), 1, 16));
    glActiveTexture(GL_TEXTURE0);
    for (int pass = 0; pass < 2; pass++) {
      glBindFramebuffer(GL_FRAMEBUFFER, m_half[1 - pass].fbo);
      glUniform2f(GLQ_LOC(kGlq, sh.id(), "step"), pass == 0 ? 1.f / hw : 0.f,
                  pass == 0 ? 0.f : 1.f / hh);
      glBindTexture(GL_TEXTURE_2D, m_half[pass].tex);
      glDrawArrays(GL_TRIANGLES, 0, 3);
    }
  }

  // add back; PS2 blending reads destination alpha, so bloom never writes it
  {
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    glViewport(0, 0, w, h);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    // hue clip: where min(frame, 1) + b exceeds 1, the shader adds only max(sum / m - frame, 0),
    // m its max channel, blended toward min(sum, 1) as m passes the white range; elsewhere a
    // plain add. Bloom never lowers a channel.
    glBlendFunc(GL_ONE, GL_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    auto& sh = rs->shaders[ShaderId::BLOOM_ADD];
    sh.activate();
    perf::set_opt_uniform(sh.id());
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "src"), 0);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "frame"), 1);
    glUniform1f(GLQ_LOC(kGlq, sh.id(), "strength"), gs.bloom_strength);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "hue_clip"), gs.bloom_hue_clip ? 1 : 0);
    glUniform2f(GLQ_LOC(kGlq, sh.id(), "white"), gs.tonemap_white_start, gs.tonemap_white_full);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_resolve.tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_half[0].tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, 0);
    glActiveTexture(GL_TEXTURE0);
  }
  prof.add_draw_call(4);
  if (timing) {
    m_timer.end();
  }
  saved.restore();
}

void BloomRenderer::draw_debug_window() {
  auto& s = Gfx::g_global_settings;
  ImGui::Checkbox("Enable bloom (emissive mask)", &s.bloom_enable);
  ImGui::SliderFloat("Strength", &s.bloom_strength, 0.f, 3.f);
  ImGui::SliderFloat("Radius (half-res px)", &s.bloom_radius, 1.f, 16.f);
  ImGui::SliderFloat("Threshold", &s.bloom_threshold, 0.f, 1.f);
  ImGui::Checkbox("Hue-preserving clip", &s.bloom_hue_clip);
  ImGui::Text("total %.3f ms (SSAO GPU timers)", m_timer.last_avg_ms());
  ImGui::Text("emissive table: %d textures", emissive_table::size());
  if (ImGui::Button("Reload emissive table")) {
    emissive_table::reload();
  }
}
