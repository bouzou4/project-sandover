#include "Smaa.h"

#include "common/log/log.h"

#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/GlQueries.h"
#include "game/graphics/opengl_renderer/PerfTimers.h"

#include "third-party/smaa/AreaTex.h"
#include "third-party/smaa/SearchTex.h"

// perf-opt bit 25: GL queries off the per-frame path (GlQueries.h)
constexpr int kGlq = PERF_OPT_PRESENT2 + 1;

namespace {
void make_target(GLuint& fbo, GLuint& tex, int w, int h, GLint internal, GLenum format,
                 GLenum type) {
  glGenFramebuffers(1, &fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, type, nullptr);
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
    lg::error("smaa: incomplete framebuffer ({}x{})", w, h);
  }
}

GLuint make_lookup(int w, int h, GLint internal, GLenum format, const unsigned char* data,
                   GLint filter) {
  GLuint tex;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
  glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_UNSIGNED_BYTE, data);
  glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  return tex;
}
}  // namespace

SmaaRenderer::~SmaaRenderer() {
  free_targets();
  for (GLuint* t : {&m_area, &m_search}) {
    if (*t) {
      glDeleteTextures(1, t);
    }
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void SmaaRenderer::free_targets() {
  for (Target* t : {&m_src, &m_edges, &m_weights}) {
    if (t->fbo) {
      glDeleteFramebuffers(1, &t->fbo);
      glDeleteTextures(1, &t->tex);
    }
    *t = Target();
  }
  m_w = m_h = 0;
}

void SmaaRenderer::ensure(int w, int h, bool hdr) {
  if (!m_area) {
    // the reference's precomputed tables: area RG8 (bilinear), search R8 (point), rows as stored
    m_area = make_lookup(AREATEX_WIDTH, AREATEX_HEIGHT, GL_RG8, GL_RG, areaTexBytes, GL_LINEAR);
    m_search =
        make_lookup(SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, GL_R8, GL_RED, searchTexBytes, GL_NEAREST);
    glGenVertexArrays(1, &m_vao);
  }
  if (w == m_w && h == m_h && hdr == m_hdr) {
    return;
  }
  free_targets();
  make_target(m_src.fbo, m_src.tex, w, h, hdr ? GL_RGBA16F : GL_RGBA8, GL_RGBA,
              hdr ? GL_HALF_FLOAT : GL_UNSIGNED_BYTE);
  make_target(m_edges.fbo, m_edges.tex, w, h, GL_RG8, GL_RG, GL_UNSIGNED_BYTE);
  make_target(m_weights.fbo, m_weights.tex, w, h, GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE);
  glBindTexture(GL_TEXTURE_2D, 0);
  m_w = w;
  m_h = h;
  m_hdr = hdr;
  lg::info("smaa: allocated targets {}x{} {}", w, h, hdr ? "RGBA16F" : "RGBA8");
}

void SmaaRenderer::render(SharedRenderState* rs, ScopedProfilerNode& prof) {
  const auto& gs = Gfx::g_global_settings;
  static glq::SavedState saved("smaa", kGlq, 0b111, 0);
  saved.capture();
  const int w = rs->render_fb_w, h = rs->render_fb_h;
  ensure(w, h, rs->hdr_target);
  glBindVertexArray(m_vao);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_SCISSOR_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_STENCIL_TEST);
  glDisable(GL_BLEND);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glViewport(0, 0, w, h);
  const float metrics[4] = {1.f / w, 1.f / h, (float)w, (float)h};
  const float kZero[4] = {0.f, 0.f, 0.f, 0.f};

  {
    // copy, or resolve at 2 samples (read buffer 0 of the render framebuffer)
    perf::Scope ps(perf::Group::SMAA_COPY);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, rs->render_fb);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_src.fbo);
    glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  }

  {
    perf::Scope ps(perf::Group::SMAA_EDGES);
    glBindFramebuffer(GL_FRAMEBUFFER, m_edges.fbo);
    glClearBufferfv(GL_COLOR, 0, kZero);
    auto& sh = rs->shaders[ShaderId::SMAA_EDGES];
    sh.activate();
    glUniform4fv(GLQ_LOC(kGlq, sh.id(), "smaa_rt_metrics"), 1, metrics);
    glUniform1f(GLQ_LOC(kGlq, sh.id(), "smaa_threshold"), gs.smaa_threshold);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "smaa_luma"), gs.smaa_luma ? 1 : 0);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "color_tex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_src.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  {
    perf::Scope ps(perf::Group::SMAA_WEIGHTS);
    glBindFramebuffer(GL_FRAMEBUFFER, m_weights.fbo);
    glClearBufferfv(GL_COLOR, 0, kZero);
    auto& sh = rs->shaders[ShaderId::SMAA_WEIGHTS];
    sh.activate();
    glUniform4fv(GLQ_LOC(kGlq, sh.id(), "smaa_rt_metrics"), 1, metrics);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "edges_tex"), 0);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "area_tex"), 1);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "search_tex"), 2);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_edges.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_area);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_search);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }

  {
    // PS2 blending reads destination alpha, so the blend writes RGB only
    perf::Scope ps(perf::Group::SMAA_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, rs->render_fb);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    auto& sh = rs->shaders[ShaderId::SMAA_BLEND];
    sh.activate();
    glUniform4fv(GLQ_LOC(kGlq, sh.id(), "smaa_rt_metrics"), 1, metrics);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "color_tex"), 0);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "blend_tex"), 1);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "edges_tex"), 2);
    glUniform1i(GLQ_LOC(kGlq, sh.id(), "smaa_debug"), gs.smaa_debug);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_src.tex);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_weights.tex);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_edges.tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  }
  prof.add_draw_call(3);
  saved.restore();
}
